// Demo: run each buggy class under contention, show the symptom, then run the fix.
//
// NOTE: the Unsafe* runs are undefined behaviour on purpose. The exact numbers change from run
// to run and between compilers, which is itself a lesson: races are non-deterministic.

#include "shared_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace roadmap::race;
using Clock = std::chrono::steady_clock;

namespace {

const int kThreads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
constexpr int kPerThread = 1'000'000;

template <typename Metrics>
void run_metrics(const char* name) {
    Metrics metrics;
    std::latch start(kThreads);
    const auto t0 = Clock::now();
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                start.arrive_and_wait();
                for (int i = 0; i < kPerThread; ++i) metrics.record(static_cast<std::uint64_t>(i % 1000));
            });
        }
    }
    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const auto expected = static_cast<std::uint64_t>(kThreads) * kPerThread;
    const MetricsSnapshot s = metrics.snapshot();
    const double lost = 100.0 * static_cast<double>(expected - std::min(expected, s.requests)) /
                        static_cast<double>(expected);
    std::println("  {:<14} requests {:>9} / {:<9} lost {:5.1f}%   max {:>3}   {:7.1f} ms   {}", name, s.requests,
                 expected, lost, s.max_latency, ms, s.requests == expected && s.max_latency == 999 ? "OK" : "WRONG");
}

template <typename Bank>
void run_bank(const char* name) {
    constexpr std::size_t accounts = 8;
    constexpr std::int64_t initial = 1'000;
    constexpr std::int64_t expected_total = static_cast<std::int64_t>(accounts) * initial;
    Bank bank(accounts, initial);
    std::atomic<bool> done{false};
    std::uint64_t audits = 0;
    std::uint64_t wrong_totals = 0;
    {
        std::jthread auditor([&] {
            while (!done.load()) {
                if (bank.total() != expected_total) ++wrong_totals;
                ++audits;
            }
        });
        {
            std::vector<std::jthread> tellers;
            for (int t = 0; t < kThreads; ++t) {
                tellers.emplace_back([&, t] {
                    std::mt19937 rng(static_cast<unsigned>(t));
                    std::uniform_int_distribution<std::size_t> account(0, accounts - 1);
                    std::uniform_int_distribution<std::int64_t> amount(1, 900);
                    for (int i = 0; i < 200'000; ++i) {
                        const std::size_t from = account(rng);
                        const std::size_t to = (from + 1 + account(rng) % (accounts - 1)) % accounts;
                        (void)bank.transfer(from, to, amount(rng));
                    }
                });
            }
        }
        done = true;
    }
    int overdrawn = 0;
    for (std::size_t a = 0; a < accounts; ++a) {
        if constexpr (requires { bank.balance(a); }) {
            if (bank.balance(a) < 0) ++overdrawn;
        } else {
            if (bank.balances()[a] < 0) ++overdrawn;
        }
    }
    std::println("  {:<16} audits {:>8}  saw wrong total {:>7} times  ({:4.1f}%)   overdrawn accounts {}", name,
                 audits, wrong_totals, 100.0 * static_cast<double>(wrong_totals) / static_cast<double>(audits),
                 overdrawn);
}

}  // namespace

int main() {
    std::println("Case 1: {} threads x {} record() calls on shared metrics", kThreads, kPerThread);
    for (int trial = 1; trial <= 3; ++trial) run_metrics<UnsafeMetrics>("UnsafeMetrics");
    run_metrics<AtomicMetrics>("AtomicMetrics");
    run_metrics<LockedMetrics>("LockedMetrics");

    std::println("\nCase 2: concurrent transfers + an auditor checking that total money never changes");
    run_bank<AtomicFieldBank>("AtomicFieldBank");  // no data race, yet still wrong
    run_bank<LockedBank>("LockedBank");

    std::println("\nCase 3: publishing a value with release/acquire + atomic wait/notify");
    OneShotPublisher<std::string> greeting;
    std::jthread reader([&] { std::println("  reader woke up and read: \"{}\"", greeting.wait()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::println("  writer publishing...");
    greeting.publish("configuration loaded: 42 settings");
}
