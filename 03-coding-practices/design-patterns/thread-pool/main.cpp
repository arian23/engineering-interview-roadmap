// Thread Pool (C++23)
//
// Idea: create a fixed set of worker threads ONCE and reuse them for many small tasks, instead of
// creating and destroying a thread per task. Thread creation costs tens of microseconds and a
// stack each; a pool also caps how many threads compete for the CPU. Theory: ../README.md#thread-pool
//
// This pool uses WORK STEALING (the design behind Java's ForkJoinPool, Intel TBB, Go's scheduler):
//   * Each worker has its own deque, so workers do not all fight over one queue lock.
//   * A task submitted FROM a worker goes to that worker's own deque; a task submitted from outside
//     goes to the workers round-robin.
//   * A worker takes work from the BACK of its own deque (LIFO: the newest task, whose data is
//     still in the CPU cache), and when its deque is empty it STEALS from the FRONT of another
//     worker's deque (FIFO: the oldest task, usually the biggest piece of remaining work).
//   * submit() returns a std::future, so results and exceptions come back to the caller.
//   * wait_idle() blocks until every submitted task, including tasks those tasks submitted, is done.
//   * The destructor finishes all queued work, then joins the workers.
// Graceful vs cancelling shutdown, bounded queues and stop tokens are covered in
// 05-system-programming/processes-threads/worker-pool.

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <print>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = std::max(1u, std::thread::hardware_concurrency()))
        : queues_(threads), executed_(threads) {
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this, i] { run(i); });
    }
    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& w : workers_) w.join();  // workers leave only after every queued task has run
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        std::packaged_task<R()> task([fn = std::forward<F>(f), ... a = std::forward<Args>(args)]() mutable { return fn(a...); });
        auto future = task.get_future();
        enqueue(std::move(task));
        return future;
    }

    /// Fire-and-forget: no future (cheaper when the result is not needed).
    template <typename F>
    void post(F&& f) {
        enqueue(std::move_only_function<void()>(std::forward<F>(f)));
    }

    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_cv_.wait(lock, [&] { return pending_ == 0; });
    }

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
    [[nodiscard]] std::uint64_t steals() const noexcept { return steals_.load(); }
    [[nodiscard]] std::vector<std::uint64_t> executed_per_worker() const {
        std::vector<std::uint64_t> out;
        for (const auto& e : executed_) out.push_back(e.load());
        return out;
    }

private:
    using Task = std::move_only_function<void()>;
    struct WorkQueue {
        std::mutex mutex;
        std::deque<Task> tasks;
    };

    void enqueue(Task task) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ && current_pool_ != this) throw std::runtime_error("ThreadPool is shutting down");
            ++pending_;  // counted BEFORE it becomes visible, so wait_idle() can never see 0 too early
            ++queued_;
        }
        // From one of our workers: keep it local. From outside: spread round-robin.
        const std::size_t target = current_pool_ == this ? current_index_ : next_.fetch_add(1) % queues_.size();
        {
            std::lock_guard lock(queues_[target].mutex);
            queues_[target].tasks.push_back(std::move(task));
        }
        work_cv_.notify_one();
    }

    bool try_take(std::size_t self, Task& out) {
        {  // 1. own deque, newest first (LIFO)
            std::lock_guard lock(queues_[self].mutex);
            if (!queues_[self].tasks.empty()) {
                out = std::move(queues_[self].tasks.back());
                queues_[self].tasks.pop_back();
                return true;
            }
        }
        for (std::size_t k = 1; k < queues_.size(); ++k) {  // 2. steal the oldest task from someone else (FIFO)
            WorkQueue& victim = queues_[(self + k) % queues_.size()];
            std::lock_guard lock(victim.mutex);
            if (!victim.tasks.empty()) {
                out = std::move(victim.tasks.front());
                victim.tasks.pop_front();
                steals_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void run(std::size_t self) {
        current_pool_ = this;
        current_index_ = self;
        for (;;) {
            Task task;
            if (try_take(self, task)) {
                {
                    std::lock_guard lock(mutex_);
                    --queued_;
                }
                task();  // packaged_task stores any exception in its future
                executed_[self].fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(mutex_);
                if (--pending_ == 0) idle_cv_.notify_all();
                continue;
            }
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [&] { return stopping_ || queued_ > 0; });
            if (stopping_ && queued_ == 0) return;
        }
    }

    std::vector<WorkQueue> queues_;  // one per worker; never resized
    std::vector<std::atomic<std::uint64_t>> executed_;
    std::mutex mutex_;  // guards pending_, queued_, stopping_
    std::condition_variable work_cv_, idle_cv_;
    std::size_t pending_ = 0;  // submitted but not finished
    std::size_t queued_ = 0;   // sitting in some deque
    bool stopping_ = false;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::uint64_t> steals_{0};
    std::vector<std::thread> workers_;  // last: threads start after everything above exists

    static thread_local const ThreadPool* current_pool_;
    static thread_local std::size_t current_index_;
};
thread_local const ThreadPool* ThreadPool::current_pool_ = nullptr;
thread_local std::size_t ThreadPool::current_index_ = 0;

// ===================================================================== demo helpers

bool is_prime(std::uint32_t n) {
    if (n < 2) return false;
    for (std::uint32_t d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}

std::uint32_t count_primes(std::uint32_t from, std::uint32_t to) {
    std::uint32_t count = 0;
    for (std::uint32_t n = from; n < to; ++n) count += is_prime(n);
    return count;
}

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

std::string join(const std::vector<std::uint64_t>& v) {
    std::string out;
    for (auto x : v) out += (out.empty() ? "" : " ") + std::to_string(x);
    return out;
}

// ===================================================================== demo

int main() {
    constexpr std::uint32_t kLimit = 4'000'000;
    constexpr std::uint32_t kChunks = 64;

    std::println("1) CPU-bound work: count primes below 4,000,000 in {} chunks", kChunks);
    auto start = Clock::now();
    const std::uint32_t serial = count_primes(0, kLimit);
    const double serial_s = seconds_since(start);
    std::println("   single thread         {:>7} primes  {:.3f} s", serial, serial_s);
    {
        ThreadPool pool;
        start = Clock::now();
        std::vector<std::future<std::uint32_t>> parts;
        for (std::uint32_t c = 0; c < kChunks; ++c) {
            parts.push_back(pool.submit(count_primes, c * (kLimit / kChunks), (c + 1) * (kLimit / kChunks)));
        }
        std::uint32_t total = 0;
        for (auto& f : parts) total += f.get();
        const double pool_s = seconds_since(start);
        std::println("   pool of {:>2} threads    {:>7} primes  {:.3f} s   speed-up x{:.1f}", pool.size(), total, pool_s, serial_s / pool_s);
        std::println("   (later chunks cost more; many small chunks keep every worker busy until the end)");
    }

    std::println("\n2) Tasks that spawn tasks: visit a binary tree with 14 levels (16,383 nodes), one task per node");
    {
        ThreadPool pool;
        std::atomic<std::uint64_t> visited{0};
        std::function<void(int)> visit = [&](int depth) {
            ++visited;
            volatile double work = 0;
            for (int i = 0; i < 2000; ++i) work = work + std::sqrt(static_cast<double>(i));  // a little CPU work per node
            if (depth < 14) {
                pool.post([&, depth] { visit(depth + 1); });  // children go to THIS worker's own deque
                pool.post([&, depth] { visit(depth + 1); });
            }
        };
        start = Clock::now();
        pool.post([&] { visit(1); });  // a single root task: other workers only get work by stealing
        pool.wait_idle();
        std::println("   visited {} nodes in {:.3f} s, {} tasks were stolen", visited.load(), seconds_since(start), pool.steals());
        std::println("   tasks run per worker: {}", join(pool.executed_per_worker()));
    }

    std::println("\n3) Exceptions travel through the future");
    {
        ThreadPool pool(2);
        auto bad = pool.submit([] { return std::stoi("not a number"); });
        try {
            (void)bad.get();
        } catch (const std::exception& e) {
            std::println("   caught in the caller: {} (the worker thread is unaffected)", e.what());
        }
    }

    std::println("\n4) Sizing: 32 I/O-bound tasks that each wait 25 ms (e.g. for a network reply)");
    for (std::size_t threads : {4u, 8u, 32u}) {
        ThreadPool pool(threads);
        start = Clock::now();
        for (int i = 0; i < 32; ++i) pool.post([] { std::this_thread::sleep_for(25ms); });
        pool.wait_idle();
        std::println("   {:>2} threads: {:.0f} ms  (ideal: 32 / {} x 25 ms = {} ms)", threads, seconds_since(start) * 1000, threads,
                     32 / threads * 25);
    }
    std::println("   CPU-bound work: about one thread per core. Waiting (I/O) work: more threads help, up to the number of waits.");
}
