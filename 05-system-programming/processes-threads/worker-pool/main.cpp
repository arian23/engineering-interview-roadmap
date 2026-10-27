// =====================================================================================================
// Worker pool with graceful shutdown (C++23)
// =====================================================================================================
//
// WHAT IT DOES
//   A fixed set of worker threads executes submitted tasks. submit() returns a std::future, so the
//   caller gets the task's result (or its exception). The pool can be shut down in two ways:
//
//     shutdown(Drain)   graceful: stop accepting tasks, finish EVERY task already queued, then join.
//     shutdown(Cancel)  fast:     stop accepting tasks, DISCARD queued tasks (their futures report
//                                 "broken promise"), ask running tasks to stop via std::stop_token,
//                                 then join.
//
// THE DESIGN
//
//      submit(f) ──► [ task queue (bounded) ] ──► worker 0 ─┐
//      submit(g) ──►        mutex + 2 condvars ──► worker 1 ─┼─► results via std::future
//                                                 worker N ─┘
//
//   * One mutex guards the queue and the `accepting_` flag.
//   * `work_available_` wakes idle workers when a task arrives or when shutdown starts.
//   * `space_available_` wakes submitters blocked on a FULL queue (backpressure: a bounded queue
//     stops a fast producer from using unbounded memory).
//
// TYPE ERASURE: how one queue holds tasks of different types
//   A task is `F(Args...) -> R`, with any F and R. We wrap it in std::packaged_task<R()>, which pairs
//   the callable with a promise and gives us the future. The queue stores
//   std::move_only_function<void(std::stop_token)>; packaged_task is move-only, so the older
//   std::function (copyable only) could not hold it. move_only_function (C++23) can.
//
// COOPERATIVE CANCELLATION
//   C++ cannot safely kill a thread. Instead every worker is a std::jthread with its own
//   std::stop_source. Tasks that accept a std::stop_token as their FIRST parameter receive the
//   worker's token and can check stop_requested() in long loops. shutdown(Cancel) calls
//   request_stop() on every worker. Tasks that ignore the token simply run to completion.
//
// THE WORKER LOOP
//   lock
//   wait until (queue not empty) OR (pool stopped accepting)
//   if queue empty  -> shutting down and nothing left: exit the thread
//   take the front task, unlock, notify one blocked submitter
//   run the task (OUTSIDE the lock, so other workers keep pulling tasks)
//
//   Note the exit condition: workers exit only when the queue is EMPTY and the pool is no longer
//   accepting. That single rule is what makes Drain finish every queued task. Cancel gets the same
//   loop to exit sooner simply by clearing the queue first.
//
// SHUTDOWN STEP BY STEP
//   1. Under the lock: accepting_ = false. From now on submit() throws, so no task can slip in.
//   2. Cancel only: move the queued tasks out and destroy them OUTSIDE the lock. Destroying a
//      packaged_task that never ran stores std::future_error(broken_promise) in its future, so
//      callers are told their task was dropped instead of waiting forever.
//      Cancel only: request_stop() on every worker's token.
//   3. notify_all on both condition variables: idle workers wake and exit; blocked submitters wake
//      and see the pool is closed.
//   4. Join every worker (jthread::join).
//   Shutdown runs once (std::call_once); the destructor calls shutdown(Drain), so a pool never
//   destroys itself while threads are still running.
//
// EXCEPTIONS
//   A throwing task never kills its worker: packaged_task catches the exception and stores it in the
//   future, and future::get() rethrows it in the caller's thread.
//
// PITFALL: never submit a task that waits on another task of the SAME pool. If every worker is busy
// waiting for queued tasks, nothing can run them: a deadlock (thread-pool starvation).
// =====================================================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace roadmap {

class WorkerPool {
public:
    enum class Shutdown { Drain, Cancel };

    struct Stats {
        std::size_t completed = 0;  // tasks that finished (normally or with an exception)
        std::size_t cancelled = 0;  // queued tasks discarded by shutdown(Cancel)
    };

    explicit WorkerPool(std::size_t threads = std::thread::hardware_concurrency(), std::size_t max_queue = 1024)
        : max_queue_(max_queue) {
        if (threads == 0 || max_queue == 0) throw std::invalid_argument("threads and max_queue must be > 0");
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this](std::stop_token stop) { worker_loop(std::move(stop)); });
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool() { shutdown(Shutdown::Drain); }

    /// Queues `f(args...)` and returns a future for its result. Blocks while the queue is full.
    /// If `f` can be called as f(std::stop_token, args...), it receives the worker's stop token.
    /// Throws std::runtime_error if the pool is shutting down.
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args) {
        constexpr bool wants_stop_token = std::is_invocable_v<std::decay_t<F>&, std::stop_token, std::decay_t<Args>&...>;
        using R = std::conditional_t<wants_stop_token,
                                     std::invoke_result<std::decay_t<F>&, std::stop_token, std::decay_t<Args>&...>,
                                     std::invoke_result<std::decay_t<F>&, std::decay_t<Args>&...>>::type;

        // packaged_task<R(stop_token)>: the token is supplied by whichever worker runs the task.
        std::packaged_task<R(std::stop_token)> task(
            [fn = std::forward<F>(f), ... captured = std::forward<Args>(args)](std::stop_token stop) mutable -> R {
                if constexpr (wants_stop_token) {
                    return std::invoke(fn, std::move(stop), captured...);
                } else {
                    return std::invoke(fn, captured...);
                }
            });
        std::future<R> result = task.get_future();

        {
            std::unique_lock lock(mutex_);
            space_available_.wait(lock, [this] { return !accepting_ || queue_.size() < max_queue_; });
            if (!accepting_) throw std::runtime_error("WorkerPool is shutting down");
            queue_.emplace_back(std::move(task));  // packaged_task is move-only: move_only_function holds it
        }
        work_available_.notify_one();
        return result;
    }

    /// Idempotent. Concurrent callers wait until the first shutdown has finished.
    void shutdown(Shutdown mode) {
        std::call_once(shutdown_once_, [&] {
            std::deque<Task> dropped;
            {
                std::lock_guard lock(mutex_);
                accepting_ = false;  // (1) nothing new can be queued after this point
                if (mode == Shutdown::Cancel) {
                    dropped.swap(queue_);  // (2) take the queued tasks out...
                    cancelled_ += dropped.size();
                }
            }
            dropped.clear();  // ...and destroy them OUTSIDE the lock: futures get broken_promise
            if (mode == Shutdown::Cancel) {
                for (auto& worker : workers_) worker.request_stop();  // running tasks may stop early
            }
            work_available_.notify_all();   // (3) wake idle workers so they can exit
            space_available_.notify_all();  //     wake blocked submitters so they can fail
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();  // (4)
            }
        });
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(mutex_);
        return {completed_, cancelled_};
    }

    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

private:
    using Task = std::move_only_function<void(std::stop_token)>;

    void worker_loop(std::stop_token stop) {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this] { return !queue_.empty() || !accepting_; });
                if (queue_.empty()) return;  // not accepting AND nothing left: this worker is done
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            space_available_.notify_one();  // a slot freed up for a blocked submitter
            task(stop);                     // run outside the lock; exceptions are stored in the future
            std::lock_guard lock(mutex_);
            ++completed_;
        }
    }

    const std::size_t max_queue_;
    mutable std::mutex mutex_;                  // guards queue_, accepting_, completed_, cancelled_
    std::condition_variable work_available_;    // a task arrived, or shutdown began
    std::condition_variable space_available_;   // the queue has room, or shutdown began
    std::deque<Task> queue_;
    bool accepting_ = true;
    std::size_t completed_ = 0;
    std::size_t cancelled_ = 0;
    std::once_flag shutdown_once_;
    std::vector<std::jthread> workers_;  // declared LAST: destroyed first, after shutdown() has joined them
};

}  // namespace roadmap

// =====================================================================================================
// Demo
// =====================================================================================================

namespace {

std::mutex print_mutex;
template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::lock_guard lock(print_mutex);
    std::println(fmt, std::forward<Args>(args)...);
}

std::string thread_name() {
    static std::mutex m;
    static std::vector<std::thread::id> ids;
    std::lock_guard lock(m);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == std::this_thread::get_id()) return std::format("w{}", i);
    }
    ids.push_back(std::this_thread::get_id());
    return std::format("w{}", ids.size() - 1);
}

}  // namespace

int main() {
    using roadmap::WorkerPool;

    // ---------------------------------------------------------------- 1. results and exceptions
    std::println("1) Results and exceptions through futures (4 workers)");
    {
        WorkerPool pool(4);
        std::vector<std::future<long long>> squares;
        for (int n = 1; n <= 8; ++n) {
            squares.push_back(pool.submit([](int x) {
                std::this_thread::sleep_for(20ms);
                return static_cast<long long>(x) * x;
            }, n));
        }
        auto failing = pool.submit([]() -> int { throw std::runtime_error("bad input"); });

        long long sum = 0;
        for (auto& f : squares) sum += f.get();
        std::println("   sum of squares 1..8 = {} (expected 204)", sum);
        try {
            (void)failing.get();
        } catch (const std::exception& e) {
            std::println("   failing task rethrew in caller: \"{}\" (its worker survived)", e.what());
        }
    }

    // ---------------------------------------------------------------- 2. graceful drain
    std::println("\n2) shutdown(Drain): 12 queued jobs, 3 workers - every queued job still runs");
    {
        WorkerPool pool(3);
        std::atomic<int> done{0};
        std::vector<std::future<void>> jobs;
        for (int id = 0; id < 12; ++id) {
            jobs.push_back(pool.submit([&done, id] {
                std::this_thread::sleep_for(30ms);
                ++done;
                log("   {} finished job {:>2}", thread_name(), id);
            }));
        }
        log("   main: {} jobs still queued, calling shutdown(Drain)", pool.queued());
        const auto start = std::chrono::steady_clock::now();
        pool.shutdown(WorkerPool::Shutdown::Drain);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::println("   drained: {}/12 jobs done in {} ms (12 jobs / 3 workers x 30 ms = ~120 ms)", done.load(), ms.count());
        try {
            (void)pool.submit([] {});
        } catch (const std::exception& e) {
            std::println("   submit after shutdown -> \"{}\"", e.what());
        }
    }

    // ---------------------------------------------------------------- 3. cancel
    std::println("\n3) shutdown(Cancel): long cooperative tasks + many queued jobs");
    {
        WorkerPool pool(2);
        std::vector<std::future<int>> long_tasks;
        for (int id = 0; id < 2; ++id) {
            // This task accepts a stop_token, so it can notice shutdown(Cancel) and stop early.
            long_tasks.push_back(pool.submit([](std::stop_token stop, int task_id) {
                int steps = 0;
                while (!stop.stop_requested() && steps < 1000) {
                    std::this_thread::sleep_for(10ms);
                    ++steps;
                }
                log("   long task {} saw stop after {} of 1000 steps", task_id, steps);
                return steps;
            }, id));
        }
        std::vector<std::future<void>> queued;
        for (int i = 0; i < 10; ++i) queued.push_back(pool.submit([] { std::this_thread::sleep_for(1s); }));

        std::this_thread::sleep_for(100ms);
        const auto start = std::chrono::steady_clock::now();
        pool.shutdown(WorkerPool::Shutdown::Cancel);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        int broken = 0;
        for (auto& f : queued) {
            try {
                f.get();
            } catch (const std::future_error& e) {
                if (e.code() == std::future_errc::broken_promise) ++broken;
            }
        }
        const auto stats = pool.stats();
        std::println("   shutdown took {} ms (without cancellation: ~10 s of work)", ms.count());
        std::println("   queued jobs dropped: {} (each future reports broken_promise: {})", stats.cancelled, broken);
        std::println("   long tasks returned {} and {} steps", long_tasks[0].get(), long_tasks[1].get());
    }

    // ---------------------------------------------------------------- 4. backpressure
    std::println("\n4) Bounded queue: 1 worker, queue limit 2 - submit() blocks when the queue is full");
    {
        WorkerPool pool(1, 2);
        for (int i = 0; i < 6; ++i) {
            const auto start = std::chrono::steady_clock::now();
            (void)pool.submit([] { std::this_thread::sleep_for(50ms); });
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::println("   submit #{} returned after {:>3} ms (queued now: {})", i, ms.count(), pool.queued());
        }
    }  // destructor = shutdown(Drain)
    std::println("\ndone");
}
