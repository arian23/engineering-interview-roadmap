// =====================================================================================================
// Producer-consumer system with backpressure (C++23)
// =====================================================================================================
//
// THE PROBLEM
//   Producers generate work faster than consumers can process it. With an UNBOUNDED queue the backlog,
//   memory use and latency all grow without limit until the process is killed (OOM) or requests time out.
//   BACKPRESSURE means the slow part of the system pushes back on the fast part so the system stays inside
//   its resource limits. The bound on the queue is what makes pushing back possible.
//
// OVERFLOW POLICIES: what a producer does when the queue is full
//   Block       wait for space. Lossless; the producer slows to the consumer's pace (flow control).
//               Right for pipelines where every item matters (log shipping, ETL, file processing).
//   Timeout     wait up to a deadline, then give up and report it. Lets the caller shed load, retry
//               later, or return "503 / try again" instead of hanging forever.
//   DropNewest  reject the new item immediately. The queue keeps the OLDEST work.
//               Right when new requests can be refused (an overloaded server returning 429).
//   DropOldest  evict the oldest queued item to make room. The queue keeps the FRESHEST work.
//               Right for telemetry and real-time state (sensor readings, prices, video frames)
//               where stale data is worthless.
//
// DESIGN
//   BackpressureQueue<T>: a bounded deque guarded by one mutex with two condition variables
//   (not_full_ for producers, not_empty_ for consumers), plus close() for shutdown. offer() applies the
//   chosen policy and returns an Outcome telling the producer exactly what happened. Counters are kept
//   under the same lock, so the statistics are exact.
//
//   high-water mark: the largest queue length ever seen. It shows the bound held (memory stayed
//   bounded) and how close the system came to it.
//
// FLOW CONTROL IN A PIPELINE
//   Stage A (fast) -> queue(4) -> Stage B (slow) -> queue(4) -> Stage C (fast)
//   When B falls behind, the A->B queue fills, A blocks, and the whole pipeline runs at B's speed
//   (the bottleneck). Memory stays bounded by the queue sizes no matter how much input arrives. This is
//   the same idea as TCP's receive window, Reactive Streams' request(n), or Go's buffered channels.
//
// SHUTDOWN
//   Producers finish -> close() the queue -> consumers drain the remaining items -> pop() returns nullopt
//   -> consumers exit. No item that was accepted is lost (except those DropOldest evicted on purpose).
// =====================================================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <print>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace roadmap {

enum class Overflow { Block, Timeout, DropNewest, DropOldest };

[[nodiscard]] constexpr std::string_view to_string(Overflow p) {
    switch (p) {
        case Overflow::Block: return "Block";
        case Overflow::Timeout: return "Timeout(5ms)";
        case Overflow::DropNewest: return "DropNewest";
        case Overflow::DropOldest: return "DropOldest";
    }
    return "?";
}

/// What happened to an offered item.
enum class Outcome { Accepted, AcceptedEvictedOldest, RejectedFull, TimedOut, Closed };

template <typename T>
class BackpressureQueue {
public:
    struct Stats {
        std::uint64_t accepted = 0, rejected = 0, timed_out = 0, evicted = 0, consumed = 0;
        std::size_t high_water = 0;
    };

    BackpressureQueue(std::size_t capacity, Overflow policy, std::chrono::milliseconds timeout = 5ms)
        : capacity_(capacity), policy_(policy), timeout_(timeout) {
        if (capacity_ == 0) throw std::invalid_argument("capacity must be > 0");
    }

    /// Producer side: applies the overflow policy.
    Outcome offer(T item) {
        std::unique_lock lock(mutex_);
        if (closed_) return Outcome::Closed;

        if (items_.size() >= capacity_) {
            switch (policy_) {
                case Overflow::Block:  // lossless: wait for a consumer to make room
                    not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
                    break;
                case Overflow::Timeout:  // bounded wait, then shed the item
                    if (!not_full_.wait_for(lock, timeout_, [this] { return closed_ || items_.size() < capacity_; })) {
                        ++stats_.timed_out;
                        return Outcome::TimedOut;
                    }
                    break;
                case Overflow::DropNewest:  // refuse the newcomer, keep the backlog
                    ++stats_.rejected;
                    return Outcome::RejectedFull;
                case Overflow::DropOldest:  // make room by discarding the stalest item
                    items_.pop_front();
                    ++stats_.evicted;
                    push_locked(std::move(item));
                    lock.unlock();
                    not_empty_.notify_one();
                    return Outcome::AcceptedEvictedOldest;
            }
            if (closed_) return Outcome::Closed;  // woke up because of shutdown, not because of space
        }

        push_locked(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return Outcome::Accepted;
    }

    /// Consumer side: blocks while empty; nullopt once the queue is closed AND drained.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        ++stats_.consumed;
        lock.unlock();
        not_full_.notify_one();  // a producer blocked on a full queue may proceed
        return item;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    void push_locked(T item) {
        items_.push_back(std::move(item));
        ++stats_.accepted;
        stats_.high_water = std::max(stats_.high_water, items_.size());
    }

    const std::size_t capacity_;
    const Overflow policy_;
    const std::chrono::milliseconds timeout_;
    mutable std::mutex mutex_;  // guards everything below
    std::condition_variable not_full_, not_empty_;
    std::deque<T> items_;
    bool closed_ = false;
    Stats stats_;
};

}  // namespace roadmap

// =====================================================================================================
// Demo
// =====================================================================================================

namespace {

using roadmap::BackpressureQueue;
using roadmap::Outcome;
using roadmap::Overflow;

struct Reading {
    int sequence;
    Clock::time_point produced_at;
};

// 2 fast producers (one item every 1 ms) and 1 slow consumer (3 ms per item), queue capacity 8.
void run_policy(Overflow policy) {
    constexpr int kProducers = 2, kPerProducer = 150;
    BackpressureQueue<Reading> queue(8, policy);
    std::atomic<std::int64_t> latency_sum_us{0}, latency_max_us{0};
    std::vector<int> consumed_sequences;

    const auto start = Clock::now();
    std::jthread consumer([&] {
        while (auto r = queue.pop()) {
            std::this_thread::sleep_for(3ms);  // the bottleneck
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - r->produced_at).count();
            latency_sum_us += us;
            if (us > latency_max_us) latency_max_us = us;
            consumed_sequences.push_back(r->sequence);
        }
    });
    {
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    (void)queue.offer({p * kPerProducer + i, Clock::now()});
                    std::this_thread::sleep_for(1ms);
                }
            });
        }
    }
    const double produce_s = std::chrono::duration<double>(Clock::now() - start).count();
    queue.close();
    consumer.join();
    const double total_s = std::chrono::duration<double>(Clock::now() - start).count();

    const auto s = queue.stats();
    const int newest_kept = std::ranges::count_if(consumed_sequences, [](int seq) { return seq % kPerProducer >= kPerProducer - 20; });
    std::println("  {:<13} accepted {:>3}  shed {:>3}  consumed {:>3}  peak queue {:>2}/8  producers done {:4.2f}s  "
                 "total {:4.2f}s  avg latency {:>4} ms  max {:>4} ms  newest-20 kept {:>2}/40",
                 to_string(policy), s.accepted, s.rejected + s.timed_out + s.evicted, s.consumed, s.high_water, produce_s,
                 total_s, s.consumed ? latency_sum_us.load() / static_cast<std::int64_t>(s.consumed) / 1000 : 0,
                 latency_max_us.load() / 1000, newest_kept);
}

}  // namespace

int main() {
    std::println("1) Same overload, four policies: 2 producers @ ~1000 items/s each, 1 consumer @ ~330 items/s, capacity 8");
    for (Overflow p : {Overflow::Block, Overflow::Timeout, Overflow::DropNewest, Overflow::DropOldest}) run_policy(p);
    std::println("   Block:      lossless, but producers are slowed to the consumer's pace (done late).");
    std::println("   Timeout:    producers never wait more than 5 ms per item; overflow is shed and reported.");
    std::println("   DropNewest: producers never wait; the queue keeps old work (newest items lost).");
    std::println("   DropOldest: producers never wait; the queue keeps the freshest data (best for telemetry).");

    std::println("\n2) A 3-stage pipeline with blocking queues: the slowest stage sets the pace, memory stays bounded");
    {
        BackpressureQueue<int> a_to_b(4, Overflow::Block), b_to_c(4, Overflow::Block);
        std::atomic<std::size_t> produced{0};
        long long checksum = 0;
        const auto start = Clock::now();
        std::jthread stage_c([&] {
            while (auto v = b_to_c.pop()) checksum += *v;  // fast sink
        });
        std::jthread stage_b([&] {
            while (auto v = a_to_b.pop()) {
                std::this_thread::sleep_for(2ms);  // slow transform: ~500 items/s
                (void)b_to_c.offer(*v * 2);
            }
            b_to_c.close();  // propagate shutdown downstream
        });
        for (int i = 1; i <= 200; ++i) {  // fast source: would produce instantly without backpressure
            (void)a_to_b.offer(i);
            ++produced;
        }
        a_to_b.close();
        stage_b.join();
        stage_c.join();
        const double secs = std::chrono::duration<double>(Clock::now() - start).count();
        std::println("   200 items through the pipeline in {:.2f}s (~{:.0f} items/s: the 2 ms stage's speed)", secs, 200 / secs);
        std::println("   checksum {} (expected {}), peak queue sizes: A->B {}/4, B->C {}/4", checksum, 200 * 201,
                     a_to_b.stats().high_water, b_to_c.stats().high_water);
    }
}
