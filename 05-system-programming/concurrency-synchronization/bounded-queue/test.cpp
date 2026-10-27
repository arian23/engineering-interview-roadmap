#include "bounded_queue.hpp"
#include "minitest.hpp"

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using roadmap::BoundedQueue;

namespace {
// Closes the queue when a test scope ends, so a failing CHECK can never leave a
// helper thread blocked forever (which would hang the jthread destructor).
template <typename T>
struct CloseOnExit {
    BoundedQueue<T>& queue;
    ~CloseOnExit() { queue.close(); }
};
}  // namespace

TEST(rejects_zero_capacity) { CHECK_THROWS(BoundedQueue<int>(0), std::invalid_argument); }

TEST(preserves_fifo_order) {
    BoundedQueue<int> q(3);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK_EQ(q.size(), 3u);
    CHECK_EQ(q.pop().value(), 1);
    CHECK_EQ(q.pop().value(), 2);
    CHECK_EQ(q.pop().value(), 3);
    CHECK(q.empty());
}

TEST(try_push_fails_when_full_without_consuming_value) {
    BoundedQueue<std::unique_ptr<int>> q(1);
    CHECK(q.try_push(std::make_unique<int>(1)));
    auto keep = std::make_unique<int>(2);
    CHECK(!q.try_push(std::move(keep)));
    CHECK(keep != nullptr);  // a failed push must not move from the argument
    CHECK_EQ(*keep, 2);
}

TEST(try_pop_on_empty_returns_nullopt) {
    BoundedQueue<std::string> q(2);
    CHECK(!q.try_pop().has_value());
    CHECK(q.try_push(std::string("x")));
    CHECK_EQ(q.try_pop().value(), std::string("x"));
}

TEST(push_blocks_while_full_until_a_pop) {
    BoundedQueue<int> q(1);
    CHECK(q.push(1));
    std::atomic<bool> pushed{false};
    std::jthread producer([&] {
        if (q.push(2)) pushed = true;
    });
    CloseOnExit<int> guard{q};

    std::this_thread::sleep_for(50ms);
    CHECK(!pushed.load());  // still blocked: the queue is full
    CHECK_EQ(q.pop().value(), 1);
    producer.join();
    CHECK(pushed.load());
    CHECK_EQ(q.pop().value(), 2);
}

TEST(pop_blocks_until_an_item_arrives) {
    BoundedQueue<int> q(1);
    std::atomic<int> received{-1};
    std::jthread consumer([&] {
        if (auto v = q.pop()) received = *v;
    });
    CloseOnExit<int> guard{q};

    std::this_thread::sleep_for(50ms);
    CHECK_EQ(received.load(), -1);
    CHECK(q.push(42));
    consumer.join();
    CHECK_EQ(received.load(), 42);
}

TEST(close_wakes_blocked_consumers) {
    BoundedQueue<int> q(1);
    std::atomic<int> woke_with_nullopt{0};
    {
        std::vector<std::jthread> consumers;
        for (int i = 0; i < 3; ++i) {
            consumers.emplace_back([&] {
                if (!q.pop()) ++woke_with_nullopt;
            });
        }
        std::this_thread::sleep_for(50ms);
        q.close();
    }
    CHECK_EQ(woke_with_nullopt.load(), 3);
}

TEST(close_wakes_blocked_producers) {
    BoundedQueue<int> q(1);
    CHECK(q.push(0));
    std::atomic<int> rejected{0};
    {
        std::vector<std::jthread> producers;
        for (int i = 0; i < 3; ++i) {
            producers.emplace_back([&] {
                if (!q.push(1)) ++rejected;
            });
        }
        std::this_thread::sleep_for(50ms);
        q.close();
    }
    CHECK_EQ(rejected.load(), 3);
}

TEST(close_drains_items_already_queued) {
    BoundedQueue<int> q(4);
    CHECK(q.push(1));
    CHECK(q.push(2));
    q.close();
    CHECK(!q.push(3));  // no new items after close
    CHECK(!q.try_push(3));
    CHECK_EQ(q.pop().value(), 1);  // ...but existing items are still delivered
    CHECK_EQ(q.pop().value(), 2);
    CHECK(!q.pop().has_value());  // closed and drained
    CHECK(q.is_closed());
}

TEST(stop_token_cancels_a_blocked_pop) {
    BoundedQueue<int> q(1);
    std::atomic<bool> returned_empty{false};
    std::jthread consumer([&](std::stop_token stop) {
        if (!q.pop(stop)) returned_empty = true;
    });
    std::this_thread::sleep_for(50ms);
    consumer.request_stop();
    consumer.join();
    CHECK(returned_empty.load());
    CHECK(!q.is_closed());  // cancelling one waiter does not close the queue
}

TEST(stop_token_cancels_a_blocked_push) {
    BoundedQueue<int> q(1);
    CHECK(q.push(1));
    std::atomic<bool> gave_up{false};
    std::jthread producer([&](std::stop_token stop) {
        if (!q.push(2, stop)) gave_up = true;
    });
    std::this_thread::sleep_for(50ms);
    producer.request_stop();
    producer.join();
    CHECK(gave_up.load());
    CHECK_EQ(q.size(), 1u);
}

TEST(timed_operations_time_out) {
    BoundedQueue<int> q(1);
    const auto start = std::chrono::steady_clock::now();
    CHECK(!q.pop_for(30ms).has_value());
    CHECK(std::chrono::steady_clock::now() - start >= 30ms);

    CHECK(q.push(1));
    CHECK(!q.push_for(2, 30ms));
    CHECK_EQ(q.size(), 1u);
}

TEST(mpmc_stress_every_item_delivered_exactly_once) {
    constexpr int producers = 4;
    constexpr int consumers = 4;
    constexpr int per_producer = 25'000;
    constexpr int total = producers * per_producer;

    BoundedQueue<int> q(8);
    std::vector<std::atomic<int>> seen(total);
    std::atomic<std::size_t> max_size_seen{0};

    {
        std::vector<std::jthread> consumer_threads;
        for (int c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&] {
                while (auto value = q.pop()) {
                    seen[static_cast<std::size_t>(*value)].fetch_add(1, std::memory_order_relaxed);
                    std::size_t now = q.size();
                    std::size_t prev = max_size_seen.load();
                    while (now > prev && !max_size_seen.compare_exchange_weak(prev, now)) {
                    }
                }
            });
        }
        {
            std::latch start(producers);
            std::vector<std::jthread> producer_threads;
            for (int p = 0; p < producers; ++p) {
                producer_threads.emplace_back([&, p] {
                    start.arrive_and_wait();  // maximise contention by starting together
                    for (int i = 0; i < per_producer; ++i) (void)q.push(p * per_producer + i);
                });
            }
        }  // all producers joined
        q.close();
    }  // all consumers joined

    int missing = 0;
    int duplicated = 0;
    for (auto& count : seen) {
        if (count == 0) ++missing;
        if (count > 1) ++duplicated;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(duplicated, 0);
    CHECK(max_size_seen.load() <= q.capacity());
}

int main() { return minitest::run_all(); }
