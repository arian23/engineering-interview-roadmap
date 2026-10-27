#include "minitest.hpp"
#include "shared_state.hpp"

#include <atomic>
#include <cstdint>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace roadmap::race;

namespace {
constexpr int kThreads = 8;
constexpr int kPerThread = 100'000;

// Every thread records latencies 0..99 repeatedly; returns the expected totals.
template <typename Metrics>
MetricsSnapshot hammer(Metrics& metrics) {
    std::latch start(kThreads);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                start.arrive_and_wait();  // release all threads at once to maximise contention
                for (int i = 0; i < kPerThread; ++i) metrics.record(static_cast<std::uint64_t>(i % 100));
            });
        }
    }
    std::uint64_t per_thread_total = 0;
    for (int i = 0; i < kPerThread; ++i) per_thread_total += static_cast<std::uint64_t>(i % 100);
    return {kThreads * static_cast<std::uint64_t>(kPerThread), kThreads * per_thread_total, 99};
}
}  // namespace

TEST(unsafe_metrics_is_correct_with_a_single_thread) {
    // The bug only exists under concurrency, which is exactly why races slip through simple tests.
    UnsafeMetrics metrics;
    for (std::uint64_t i = 1; i <= 10; ++i) metrics.record(i);
    CHECK_EQ(metrics.snapshot().requests, 10u);
    CHECK_EQ(metrics.snapshot().total_latency, 55u);
    CHECK_EQ(metrics.snapshot().max_latency, 10u);
}

TEST(atomic_metrics_lose_no_updates) {
    AtomicMetrics metrics;
    const MetricsSnapshot expected = hammer(metrics);
    const MetricsSnapshot actual = metrics.snapshot();
    CHECK_EQ(actual.requests, expected.requests);
    CHECK_EQ(actual.total_latency, expected.total_latency);
    CHECK_EQ(actual.max_latency, expected.max_latency);
}

TEST(locked_metrics_lose_no_updates) {
    LockedMetrics metrics;
    const MetricsSnapshot expected = hammer(metrics);
    const MetricsSnapshot actual = metrics.snapshot();
    CHECK_EQ(actual.requests, expected.requests);
    CHECK_EQ(actual.total_latency, expected.total_latency);
    CHECK_EQ(actual.max_latency, expected.max_latency);
}

TEST(atomic_max_survives_contention) {
    // Threads race to publish different maxima; the CAS loop must keep the largest.
    AtomicMetrics metrics;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (std::uint64_t v = 0; v < 50'000; ++v) metrics.record(v * kThreads + static_cast<std::uint64_t>(t));
            });
        }
    }
    CHECK_EQ(metrics.snapshot().max_latency, 49'999u * kThreads + (kThreads - 1));
}

TEST(locked_metrics_snapshots_are_consistent) {
    // Writers always record latency 10, so a consistent snapshot has total == 10 * requests.
    LockedMetrics metrics;
    std::atomic<bool> done{false};
    std::uint64_t inconsistent = 0;
    std::uint64_t snapshots = 0;
    {
        std::vector<std::jthread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&] {
                for (int i = 0; i < 100'000; ++i) metrics.record(10);
            });
        }
        std::jthread reader([&] {
            while (!done.load()) {
                const MetricsSnapshot s = metrics.snapshot();
                if (s.total_latency != 10 * s.requests) ++inconsistent;
                ++snapshots;
            }
        });
        for (auto& w : writers) w.join();
        done = true;
    }
    CHECK(snapshots > 0);
    CHECK_EQ(inconsistent, 0u);
}

TEST(locked_bank_never_creates_or_destroys_money) {
    constexpr std::size_t accounts = 10;
    constexpr std::int64_t initial = 1'000;
    LockedBank bank(accounts, initial);
    std::atomic<bool> done{false};
    std::uint64_t bad_audits = 0;
    std::uint64_t audits = 0;
    {
        std::jthread auditor([&] {
            while (!done.load()) {
                if (bank.total() != static_cast<std::int64_t>(accounts) * initial) ++bad_audits;
                ++audits;
            }
        });
        {
            std::vector<std::jthread> tellers;
            for (int t = 0; t < kThreads; ++t) {
                tellers.emplace_back([&, t] {
                    std::mt19937 rng(static_cast<unsigned>(t));
                    std::uniform_int_distribution<std::size_t> account(0, accounts - 1);
                    std::uniform_int_distribution<std::int64_t> amount(1, 300);
                    for (int i = 0; i < 50'000; ++i) (void)bank.transfer(account(rng), account(rng), amount(rng));
                });
            }
        }
        done = true;
    }
    CHECK(audits > 0);
    CHECK_EQ(bad_audits, 0u);
    CHECK_EQ(bank.total(), static_cast<std::int64_t>(accounts) * initial);
    for (std::int64_t balance : bank.balances()) CHECK(balance >= 0);  // no overdraft ever committed
}

TEST(locked_bank_validates_input) {
    LockedBank bank(2, 100);
    CHECK(!bank.transfer(0, 1, 101));  // insufficient funds
    CHECK(!bank.transfer(0, 0, 10));   // same account
    CHECK(bank.transfer(0, 1, 100));
    CHECK_EQ(bank.balances()[0], 0);
    CHECK_EQ(bank.balances()[1], 200);
    CHECK_THROWS(bank.transfer(0, 1, 0), std::invalid_argument);
    CHECK_THROWS(bank.transfer(0, 5, 1), std::out_of_range);
}

TEST(publisher_makes_the_whole_value_visible) {
    OneShotPublisher<std::vector<std::string>> config;
    constexpr int readers = 6;
    std::atomic<int> complete{0};
    {
        std::vector<std::jthread> threads;
        for (int r = 0; r < readers; ++r) {
            threads.emplace_back([&] {
                const auto& value = config.wait();  // blocks until published
                if (value.size() == 3 && value[2] == "gamma") ++complete;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // readers are now blocked
        CHECK(!config.is_published());
        config.publish({"alpha", "beta", "gamma"});
    }
    CHECK_EQ(complete.load(), readers);
}

TEST(publisher_rejects_a_second_publish) {
    OneShotPublisher<int> value;
    value.publish(1);
    CHECK_THROWS(value.publish(2), std::logic_error);
    CHECK_EQ(value.wait(), 1);
}

int main() { return minitest::run_all(); }
