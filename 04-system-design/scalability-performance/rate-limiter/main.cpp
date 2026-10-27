// Demo: compare the five algorithms on the same traffic, then rate-limit several clients per key,
// then use retry_after with the real clock.

#include "rate_limiter.hpp"

#include <chrono>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace roadmap::rate;

namespace {

// Runs the same scenario against every algorithm: limit 10 requests per second.
template <typename Scenario>
void compare(const char* title, Scenario scenario) {
    std::println("{}", title);
    auto run = [&](const char* name, auto make) {
        ManualClock clock;
        auto limiter = make(clock);
        std::println("  {:<22} {}", name, scenario(limiter, clock));
    };
    run("TokenBucket(10, 10/s)", [](ManualClock c) { return TokenBucket(10, Rate{10, 1s}, c); });
    run("GcraLimiter(10, 10/s)", [](ManualClock c) { return GcraLimiter(10, Rate{10, 1s}, c); });
    run("FixedWindow(10/1s)", [](ManualClock c) { return FixedWindowCounter(10, 1s, c); });
    run("SlidingLog(10/1s)", [](ManualClock c) { return SlidingWindowLog(10, 1s, c); });
    run("SlidingCounter(10/1s)", [](ManualClock c) { return SlidingWindowCounter(10, 1s, c); });
    std::println("");
}

template <typename Limiter>
int burst(Limiter& limiter, int requests) {
    int allowed = 0;
    for (int i = 0; i < requests; ++i) allowed += limiter.try_acquire() ? 1 : 0;
    return allowed;
}

}  // namespace

int main() {
    // 1. Boundary burst: 15 requests at t=0.9 s, 15 more at t=1.0 s.
    compare("1) Boundary burst: 15 requests at t=0.9s, then 15 at t=1.0s (limit 10/s)",
            [](auto& limiter, ManualClock& clock) {
                clock.advance(900ms);
                const int first = burst(limiter, 15);
                clock.advance(100ms);
                const int second = burst(limiter, 15);
                return std::format("allowed {:>2} + {:>2} = {:>2} within 100 ms", first, second, first + second);
            });

    // 2. A client sending 20 requests per second for 3 seconds.
    compare("2) Steady overload: one request every 50 ms for 3 s (20/s offered, limit 10/s)",
            [](auto& limiter, ManualClock& clock) {
                std::vector<int> per_second(3, 0);
                for (int tick = 0; tick < 60; ++tick) {
                    if (limiter.try_acquire()) ++per_second[static_cast<std::size_t>(tick / 20)];
                    clock.advance(50ms);
                }
                return std::format("allowed per second: {} / {} / {}", per_second[0], per_second[1], per_second[2]);
            });

    // 3. Per-client limits: each key gets its own token bucket (burst 5, 5/s).
    std::println("3) Per-client limits (burst 5, 5 requests/s per client), 4 simulated seconds");
    {
        ManualClock clock;
        using Bucket = TokenBucket<ManualClock>;
        KeyedRateLimiter<Bucket, std::string, ManualClock> limiter(
            [clock] { return std::make_unique<Bucket>(5, Rate{5, 1s}, clock); }, 16, clock);
        struct Client {
            std::string name;
            int every_ms;  // sends one request every N ms
            int allowed = 0;
            int denied = 0;
        };
        std::vector<Client> clients{{"alice (4/s)", 250}, {"bob (8/s)", 125}, {"mallory (100/s)", 10}};
        for (int ms = 0; ms < 4000; ms += 5) {
            for (Client& c : clients) {
                if (ms % c.every_ms != 0) continue;
                (limiter.try_acquire(c.name) ? c.allowed : c.denied)++;
            }
            clock.advance(5ms);
        }
        for (const Client& c : clients) {
            std::println("  {:<16} allowed {:>3}   denied {:>3}", c.name, c.allowed, c.denied);
        }
        std::println("  tracked keys: {}\n", limiter.size());
    }

    // 4. Real time: a well-behaved client that sleeps for retry_after instead of hammering.
    std::println("4) Real clock: 25 requests through TokenBucket(burst 5, 20/s), sleeping for retry_after");
    TokenBucket bucket(5, Rate{20, 1s});
    const auto start = std::chrono::steady_clock::now();
    int sleeps = 0;
    for (int sent = 0; sent < 25;) {
        if (Decision d = bucket.try_acquire()) {
            ++sent;
        } else {
            ++sleeps;
            std::this_thread::sleep_for(d.retry_after);
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::println("  25 requests in {:.2f} s (expected ~1.0 s: 5 burst + 20 at 20/s), {} waits", elapsed, sleeps);
}
