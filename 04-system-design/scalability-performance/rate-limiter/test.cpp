#include "minitest.hpp"
#include "rate_limiter.hpp"

#include <array>
#include <atomic>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace roadmap::rate;

namespace {

// Hammers `limiter` from many threads at one frozen instant; returns how many permits were granted.
template <typename Limiter>
std::uint64_t allowed_under_contention(Limiter& limiter, int threads = 8, int attempts = 5'000) {
    std::atomic<std::uint64_t> allowed{0};
    std::latch start(threads);
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                start.arrive_and_wait();
                for (int i = 0; i < attempts; ++i) {
                    if (limiter.try_acquire()) allowed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    return allowed.load();
}

// Saturates the limiter, then checks that waiting exactly retry_after is enough (and, for exact
// algorithms, that waiting 1 ns less is not).
template <typename Limiter>
void check_retry_after_is_honoured(Limiter& limiter, ManualClock& clock, bool exact) {
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> gap_ms(0, 150);
    for (int round = 0; round < 300; ++round) {
        clock.advance(std::chrono::milliseconds(gap_ms(rng)));
        Decision d = limiter.try_acquire();
        if (d) continue;
        CHECK(d.retry_after > Nanos::zero());
        if (exact && d.retry_after > Nanos(1)) {
            clock.advance(d.retry_after - Nanos(1));
            CHECK(!limiter.try_acquire());  // 1 ns too early
            clock.advance(Nanos(1));
        } else {
            clock.advance(d.retry_after);
        }
        CHECK(limiter.try_acquire());
    }
}

}  // namespace

// ------------------------------------------------------------------------------- configuration

TEST(rate_and_constructor_validation) {
    CHECK_THROWS((Rate{0, 1s}.interval()), std::invalid_argument);
    CHECK_THROWS((Rate{1, 0s}.interval()), std::invalid_argument);
    CHECK_EQ((Rate{10, 1s}.interval()), Nanos(100ms));  // extra parentheses: the macro would split on the comma
    CHECK_THROWS((TokenBucket(0, Rate{1, 1s})), std::invalid_argument);
    CHECK_THROWS((FixedWindowCounter(5, 0s)), std::invalid_argument);
    TokenBucket bucket(3, Rate{1, 1s});
    CHECK_THROWS(bucket.try_acquire(4), std::invalid_argument);  // could never succeed
}

// ------------------------------------------------------------------------------- token bucket

TEST(token_bucket_allows_burst_then_enforces_rate) {
    ManualClock clock;
    TokenBucket bucket(5, Rate{10, 1s}, clock);  // burst of 5, then one permit every 100 ms
    for (int i = 0; i < 5; ++i) CHECK(bucket.try_acquire());

    const Decision denied = bucket.try_acquire();
    CHECK(!denied);
    CHECK_EQ(denied.retry_after, Nanos(100ms));

    clock.advance(99ms);
    CHECK(!bucket.try_acquire());
    clock.advance(1ms);
    CHECK(bucket.try_acquire());
    CHECK(!bucket.try_acquire());
}

TEST(token_bucket_refill_is_capped_at_capacity) {
    ManualClock clock;
    TokenBucket bucket(3, Rate{1, 1s}, clock);
    for (int i = 0; i < 3; ++i) CHECK(bucket.try_acquire());
    clock.advance(1h);  // a long idle period must not bank more than `capacity` tokens
    CHECK_EQ(bucket.available_tokens(), 3.0);
    for (int i = 0; i < 3; ++i) CHECK(bucket.try_acquire());
    CHECK(!bucket.try_acquire());
}

TEST(token_bucket_supports_weighted_requests) {
    ManualClock clock;
    TokenBucket bucket(10, Rate{10, 1s}, clock);
    CHECK(bucket.try_acquire(7));
    const Decision denied = bucket.try_acquire(4);  // 3 left, needs 4
    CHECK(!denied);
    CHECK_EQ(denied.retry_after, Nanos(100ms));
    CHECK(bucket.try_acquire(3));  // a failed request consumed nothing
}

// ------------------------------------------------------------------------------- GCRA

TEST(gcra_matches_token_bucket_exactly) {
    ManualClock clock;
    TokenBucket bucket(4, Rate{3, 1s}, clock);
    GcraLimiter gcra(4, Rate{3, 1s}, clock);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> gap_ms(0, 400);
    std::uniform_int_distribution<std::uint32_t> cost(1, 3);
    for (int step = 0; step < 20'000; ++step) {
        clock.advance(std::chrono::milliseconds(gap_ms(rng)));
        const std::uint32_t c = cost(rng);
        const Decision a = bucket.try_acquire(c);
        const Decision b = gcra.try_acquire(c);
        CHECK_EQ(a.allowed, b.allowed);
        CHECK_EQ(a.retry_after, b.retry_after);
    }
}

// ------------------------------------------------------------------------------- windows

TEST(fixed_window_resets_at_boundary_and_allows_a_double_burst) {
    ManualClock clock;
    FixedWindowCounter limiter(10, 1s, clock);
    clock.advance(900ms);
    for (int i = 0; i < 10; ++i) CHECK(limiter.try_acquire());
    const Decision denied = limiter.try_acquire();
    CHECK(!denied);
    CHECK_EQ(denied.retry_after, Nanos(100ms));

    clock.advance(100ms);  // t = 1.000 s: a brand-new window
    for (int i = 0; i < 10; ++i) CHECK(limiter.try_acquire());  // 20 permits within 100 ms: the known weakness
    CHECK(!limiter.try_acquire());
}

TEST(sliding_log_blocks_the_boundary_burst) {
    ManualClock clock;
    SlidingWindowLog limiter(10, 1s, clock);
    clock.advance(900ms);
    for (int i = 0; i < 10; ++i) CHECK(limiter.try_acquire());
    CHECK_EQ(limiter.try_acquire().retry_after, Nanos(1s));  // oldest permit (t=0.9) leaves at t=1.9

    clock.advance(100ms);  // t = 1.0: all 10 permits are still inside the last second
    const Decision denied = limiter.try_acquire();
    CHECK(!denied);
    CHECK_EQ(denied.retry_after, Nanos(900ms));

    clock.advance(900ms);  // t = 1.9
    for (int i = 0; i < 10; ++i) CHECK(limiter.try_acquire());
    CHECK(!limiter.try_acquire());
}

TEST(sliding_log_never_exceeds_limit_in_any_rolling_window) {
    constexpr std::uint32_t limit = 5;
    constexpr Nanos window = 1s;
    ManualClock clock;
    SlidingWindowLog limiter(limit, window, clock);
    std::mt19937 rng(3);
    std::uniform_int_distribution<int> gap_ms(0, 120);
    std::vector<TimePoint> granted;
    for (int i = 0; i < 20'000; ++i) {
        clock.advance(std::chrono::milliseconds(gap_ms(rng)));
        if (limiter.try_acquire()) granted.push_back(clock.now());
    }
    // Two pointers: for every grant, count grants in the half-open window (t - window, t].
    std::size_t oldest = 0;
    std::size_t worst = 0;
    for (std::size_t newest = 0; newest < granted.size(); ++newest) {
        while (granted[oldest] <= granted[newest] - window) ++oldest;
        worst = std::max(worst, newest - oldest + 1);
    }
    CHECK_EQ(worst, limit);
}

TEST(sliding_counter_smooths_the_boundary_burst) {
    ManualClock clock;
    SlidingWindowCounter limiter(10, 1s, clock);
    clock.advance(900ms);
    for (int i = 0; i < 10; ++i) CHECK(limiter.try_acquire());

    clock.advance(100ms);           // t = 1.0: estimate = 10 * (1 - 0.0) + 0 = 10
    CHECK(!limiter.try_acquire());

    clock.advance(500ms);           // t = 1.5: estimate = 10 * (1 - 0.5) + 0 = 5
    for (int i = 0; i < 5; ++i) CHECK(limiter.try_acquire());
    const Decision denied = limiter.try_acquire();  // estimate = 5 + 5 = 10
    CHECK(!denied);
    CHECK(denied.retry_after >= Nanos(99ms) && denied.retry_after <= Nanos(101ms));  // fits once f >= 0.6
    clock.advance(denied.retry_after);
    CHECK(limiter.try_acquire());
}

TEST(sliding_counter_retry_after_accounts_for_carry_over) {
    // Regression: a full CURRENT window carries into the next one with weight ~1, so "wait for the next
    // window" is not enough. Limit 5: fill the window at t=0.6 s; at t=1.0 the estimate is still 5.
    ManualClock clock;
    SlidingWindowCounter limiter(5, 1s, clock);
    clock.advance(600ms);
    for (int i = 0; i < 5; ++i) CHECK(limiter.try_acquire());
    const Decision denied = limiter.try_acquire();
    CHECK(!denied);
    CHECK(denied.retry_after > Nanos(400ms));  // later than the window boundary (t = 1.0 s)...
    clock.advance(400ms);
    CHECK(!limiter.try_acquire());             // ...because at t = 1.0 the estimate is still 5
    clock.advance(denied.retry_after - 400ms);
    CHECK(limiter.try_acquire());              // at t = 1.2 the estimate is 5 * 0.8 = 4
}

TEST(sliding_counter_forgets_everything_after_two_idle_windows) {
    ManualClock clock;
    SlidingWindowCounter limiter(3, 1s, clock);
    for (int i = 0; i < 3; ++i) CHECK(limiter.try_acquire());
    clock.advance(2500ms);  // skipped a whole window: the previous window count must not carry over
    for (int i = 0; i < 3; ++i) CHECK(limiter.try_acquire());
}

TEST(retry_after_is_honoured_by_every_algorithm) {
    {
        ManualClock clock;
        TokenBucket limiter(3, Rate{5, 1s}, clock);
        check_retry_after_is_honoured(limiter, clock, true);
    }
    {
        ManualClock clock;
        GcraLimiter limiter(3, Rate{5, 1s}, clock);
        check_retry_after_is_honoured(limiter, clock, true);
    }
    {
        ManualClock clock;
        FixedWindowCounter limiter(5, 1s, clock);
        check_retry_after_is_honoured(limiter, clock, true);
    }
    {
        ManualClock clock;
        SlidingWindowLog limiter(5, 1s, clock);
        check_retry_after_is_honoured(limiter, clock, true);
    }
    {
        ManualClock clock;
        SlidingWindowCounter limiter(5, 1s, clock);
        check_retry_after_is_honoured(limiter, clock, false);  // approximate: only "enough" is promised
    }
}

// ------------------------------------------------------------------------------- concurrency

TEST(no_algorithm_over_admits_under_contention) {
    // The clock is frozen, so exactly `capacity` permits may be granted no matter how many threads race.
    ManualClock clock;
    TokenBucket bucket(1'000, Rate{1, 1s}, clock);
    GcraLimiter gcra(1'000, Rate{1, 1s}, clock);
    FixedWindowCounter fixed(1'000, 1s, clock);
    SlidingWindowLog log(1'000, 1s, clock);
    SlidingWindowCounter sliding(1'000, 1s, clock);
    CHECK_EQ(allowed_under_contention(bucket), 1'000u);
    CHECK_EQ(allowed_under_contention(gcra), 1'000u);
    CHECK_EQ(allowed_under_contention(fixed), 1'000u);
    CHECK_EQ(allowed_under_contention(log), 1'000u);
    CHECK_EQ(allowed_under_contention(sliding), 1'000u);
}

// ------------------------------------------------------------------------------- per-key limiter

using Bucket = TokenBucket<ManualClock>;
using PerKey = KeyedRateLimiter<Bucket, std::string, ManualClock>;

TEST(keyed_limiter_isolates_keys) {
    ManualClock clock;
    PerKey limiter([clock] { return std::make_unique<Bucket>(2, Rate{1, 1s}, clock); }, 16, clock);
    CHECK(limiter.try_acquire("alice"));
    CHECK(limiter.try_acquire("alice"));
    CHECK(!limiter.try_acquire("alice"));  // alice is out of tokens...
    CHECK(limiter.try_acquire("bob"));     // ...which does not affect bob
    CHECK(limiter.try_acquire("bob"));
    CHECK_EQ(limiter.size(), 2u);
}

TEST(keyed_limiter_evicts_idle_keys) {
    ManualClock clock;
    PerKey limiter([clock] { return std::make_unique<Bucket>(2, Rate{1, 1s}, clock); }, 4, clock);
    CHECK(limiter.try_acquire("alice"));
    clock.advance(30s);
    CHECK(limiter.try_acquire("bob"));
    CHECK_EQ(limiter.evict_idle(10s), 1u);  // alice was idle for 30 s
    CHECK_EQ(limiter.size(), 1u);
}

TEST(keyed_limiter_is_exact_per_key_under_contention) {
    constexpr int keys = 16;
    static constexpr std::uint32_t per_key = 50;  // static: usable inside the lambda without a capture
    ManualClock clock;  // frozen
    PerKey limiter([clock] { return std::make_unique<Bucket>(per_key, Rate{1, 1s}, clock); }, 8, clock);
    std::array<std::atomic<int>, keys> allowed{};
    std::latch start(8);
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < 8; ++t) {
            workers.emplace_back([&] {
                start.arrive_and_wait();
                for (int round = 0; round < 200; ++round) {
                    for (int k = 0; k < keys; ++k) {
                        if (limiter.try_acquire("client-" + std::to_string(k))) ++allowed[static_cast<std::size_t>(k)];
                    }
                }
            });
        }
    }
    for (const auto& count : allowed) CHECK_EQ(count.load(), static_cast<int>(per_key));
    CHECK_EQ(limiter.size(), static_cast<std::size_t>(keys));
}

TEST(keyed_limiter_factory_failure_leaves_no_entry) {
    ManualClock clock;
    PerKey limiter([]() -> std::unique_ptr<Bucket> { throw std::runtime_error("no memory"); }, 4, clock);
    CHECK_THROWS(limiter.try_acquire("alice"), std::runtime_error);
    CHECK_EQ(limiter.size(), 0u);
}

int main() { return minitest::run_all(); }
