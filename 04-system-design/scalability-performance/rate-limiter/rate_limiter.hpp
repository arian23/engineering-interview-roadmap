#pragma once
// Rate limiters: five algorithms behind one interface, plus a sharded per-key limiter.
//
//   Decision d = limiter.try_acquire();          // or try_acquire(cost)
//   if (!d) reply_429(d.retry_after);            // "Too Many Requests", Retry-After header
//
//   Algorithm              Memory/key   Exact?   Bursts                         Thread safety
//   TokenBucket            O(1)         yes      up to `capacity`, then rate    mutex
//   GcraLimiter            O(1)         yes      identical to TokenBucket       lock-free (one CAS)
//   FixedWindowCounter     O(1)         no*      up to 2x limit at a boundary   mutex
//   SlidingWindowLog       O(limit)     yes      never more than limit/window   mutex
//   SlidingWindowCounter   O(1)         approx   smoothed                       mutex
//   (* exact per window, but windows are aligned, so a burst can straddle two of them)
//
// Every limiter takes a Clock, so tests can use ManualClock and control time exactly.
// See README.md for how each algorithm works and when to use it.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace roadmap::rate {

using TimePoint = std::chrono::steady_clock::time_point;
using Nanos = std::chrono::nanoseconds;

/// Anything with a const now() returning a steady_clock time point.
/// std::chrono::steady_clock satisfies it; so does ManualClock below.
template <typename C>
concept ClockSource = requires(const C& clock) {
    { clock.now() } -> std::same_as<TimePoint>;
};

/// A clock that only moves when told to. Copies share the same time, so one ManualClock can drive
/// many limiters. Thread-safe.
class ManualClock {
public:
    [[nodiscard]] TimePoint now() const noexcept {
        return TimePoint(std::chrono::duration_cast<TimePoint::duration>(Nanos(ticks_->load(std::memory_order_relaxed))));
    }
    void advance(Nanos step) noexcept { ticks_->fetch_add(step.count(), std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<Nanos::rep>> ticks_ = std::make_shared<std::atomic<Nanos::rep>>(0);
};

/// The result of an acquire attempt.
struct Decision {
    bool allowed = false;
    Nanos retry_after{0};  // when denied: how long until the same request could succeed (0 when allowed)

    [[nodiscard]] explicit operator bool() const noexcept { return allowed; }
};

template <typename L>
concept RateLimiter = requires(L& limiter, std::uint32_t cost) {
    { limiter.try_acquire(cost) } -> std::same_as<Decision>;
};

/// "permits per period", e.g. Rate{100, 1s}. interval() is the time that earns one permit.
struct Rate {
    std::uint32_t permits = 0;
    Nanos per{0};

    [[nodiscard]] Nanos interval() const {
        if (permits == 0 || per <= Nanos::zero()) throw std::invalid_argument("rate must be positive");
        const Nanos one = per / permits;
        if (one <= Nanos::zero()) throw std::invalid_argument("rate too high: more than one permit per nanosecond");
        return one;
    }
};

namespace detail {

inline std::uint32_t require_positive(std::uint32_t value, const char* what) {
    if (value == 0) throw std::invalid_argument(std::string(what) + " must be greater than zero");
    return value;
}

inline Nanos require_positive(Nanos value, const char* what) {
    if (value <= Nanos::zero()) throw std::invalid_argument(std::string(what) + " must be positive");
    return value;
}

/// interval * count, refusing to overflow 64-bit nanoseconds (~292 years).
inline Nanos checked_multiply(Nanos interval, std::uint32_t count) {
    if (interval.count() > std::numeric_limits<Nanos::rep>::max() / static_cast<Nanos::rep>(count)) {
        throw std::invalid_argument("rate limiter configuration overflows 64-bit nanoseconds");
    }
    return interval * static_cast<Nanos::rep>(count);
}

/// A request that costs more than the limiter can ever hold would wait forever: a caller bug.
inline void check_cost(std::uint32_t cost, std::uint32_t capacity) {
    if (cost > capacity) throw std::invalid_argument("cost exceeds the limiter's capacity: it could never succeed");
}

/// Start of the window containing `t`. Windows are aligned to the clock's epoch.
inline TimePoint window_start(TimePoint t, Nanos window) { return t - (t.time_since_epoch() % window); }

inline Nanos::rep ticks(TimePoint t) { return std::chrono::duration_cast<Nanos>(t.time_since_epoch()).count(); }

}  // namespace detail

// =====================================================================================
// Token bucket
// =====================================================================================

/// A bucket holds up to `capacity` tokens and refills at `refill` rate. Each request takes `cost`
/// tokens. Allows short bursts (a full bucket) while enforcing the long-run average rate.
///
/// Implementation detail: tokens are stored as "credit" in integer nanoseconds (1 token =
/// interval() of credit, and credit grows 1 ns per ns of elapsed time). No floating point,
/// so there is no rounding drift.
template <ClockSource Clock = std::chrono::steady_clock>
class TokenBucket {
public:
    TokenBucket(std::uint32_t capacity, Rate refill, Clock clock = {})
        : capacity_(detail::require_positive(capacity, "capacity")),
          interval_(refill.interval()),
          max_credit_(detail::checked_multiply(interval_, capacity_)),
          clock_(std::move(clock)),
          credit_(max_credit_),  // start full
          last_refill_(clock_.now()) {}

    [[nodiscard]] Decision try_acquire(std::uint32_t cost = 1) {
        detail::check_cost(cost, capacity_);
        std::lock_guard lock(mutex_);
        refill(clock_.now());  // read the clock UNDER the lock so time never appears to go backwards
        const Nanos needed = interval_ * static_cast<Nanos::rep>(cost);
        if (credit_ >= needed) {
            credit_ -= needed;
            return {true, Nanos::zero()};
        }
        return {false, needed - credit_};  // exactly the time it takes to earn the missing credit
    }

    /// Tokens currently available (fractional), after refilling.
    [[nodiscard]] double available_tokens() const {
        std::lock_guard lock(mutex_);
        refill(clock_.now());
        return static_cast<double>(credit_.count()) / static_cast<double>(interval_.count());
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
    void refill(TimePoint now) const {
        if (now <= last_refill_) return;
        const Nanos elapsed = now - last_refill_;
        credit_ = (elapsed >= max_credit_ - credit_) ? max_credit_ : credit_ + elapsed;  // capped, overflow-safe
        last_refill_ = now;
    }

    const std::uint32_t capacity_;
    const Nanos interval_;
    const Nanos max_credit_;
    Clock clock_;
    mutable std::mutex mutex_;  // guards credit_ and last_refill_
    mutable Nanos credit_;
    mutable TimePoint last_refill_;
};

// =====================================================================================
// GCRA: Generic Cell Rate Algorithm (a lock-free token bucket)
// =====================================================================================

/// Mathematically identical to TokenBucket (same decisions, same retry_after), but the whole
/// state is ONE integer: the "theoretical arrival time" (TAT) of the next request if traffic
/// arrived exactly at the allowed rate. That makes it lock-free with a single CAS, and is why it is
/// popular for distributed limiters (one value per key in Redis).
///
///   allow if  max(TAT, now) + cost * interval - burst * interval  <=  now
///   then      TAT = max(TAT, now) + cost * interval
template <ClockSource Clock = std::chrono::steady_clock>
class GcraLimiter {
public:
    GcraLimiter(std::uint32_t burst, Rate rate, Clock clock = {})
        : burst_(detail::require_positive(burst, "burst")),
          interval_(rate.interval()),
          tolerance_(detail::checked_multiply(interval_, burst_)),
          clock_(std::move(clock)) {}

    [[nodiscard]] Decision try_acquire(std::uint32_t cost = 1) {
        detail::check_cost(cost, burst_);
        const Nanos::rep now = detail::ticks(clock_.now());
        const Nanos::rep increment = interval_.count() * static_cast<Nanos::rep>(cost);

        Nanos::rep tat = tat_.load(std::memory_order_relaxed);
        for (;;) {
            const Nanos::rep new_tat = std::max(tat, now) + increment;
            const Nanos::rep allow_at = new_tat - tolerance_.count();
            if (allow_at > now) return {false, Nanos(allow_at - now)};
            // Publish our new TAT only if nobody changed it since we read it; otherwise `tat` is
            // refreshed with the current value and we decide again. relaxed is enough: tat_ is the
            // only shared state, it does not guard other memory.
            if (tat_.compare_exchange_weak(tat, new_tat, std::memory_order_relaxed)) return {true, Nanos::zero()};
        }
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return burst_; }

private:
    const std::uint32_t burst_;
    const Nanos interval_;
    const Nanos tolerance_;
    Clock clock_;
    std::atomic<Nanos::rep> tat_{std::numeric_limits<Nanos::rep>::min()};  // "long ago": full burst available
};

// =====================================================================================
// Fixed window counter
// =====================================================================================

/// At most `limit` permits per aligned window [k*window, (k+1)*window). Cheapest possible state,
/// but a client can send `limit` at the end of one window and `limit` at the start of the next.
template <ClockSource Clock = std::chrono::steady_clock>
class FixedWindowCounter {
public:
    FixedWindowCounter(std::uint32_t limit, Nanos window, Clock clock = {})
        : limit_(detail::require_positive(limit, "limit")),
          window_(detail::require_positive(window, "window")),
          clock_(std::move(clock)),
          current_start_(detail::window_start(clock_.now(), window_)) {}

    [[nodiscard]] Decision try_acquire(std::uint32_t cost = 1) {
        detail::check_cost(cost, limit_);
        std::lock_guard lock(mutex_);
        const TimePoint now = clock_.now();
        if (const TimePoint start = detail::window_start(now, window_); start != current_start_) {
            current_start_ = start;  // a new window: forget the old count
            used_ = 0;
        }
        if (used_ + cost <= limit_) {
            used_ += cost;
            return {true, Nanos::zero()};
        }
        return {false, current_start_ + window_ - now};  // wait for the next window
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return limit_; }

private:
    const std::uint32_t limit_;
    const Nanos window_;
    Clock clock_;
    std::mutex mutex_;  // guards current_start_ and used_
    TimePoint current_start_;
    std::uint64_t used_ = 0;
};

// =====================================================================================
// Sliding window log
// =====================================================================================

/// Remembers the timestamp of every permit granted in the last `window`. Exact: no rolling window
/// of length `window` ever contains more than `limit` permits. Costs O(limit) memory per limiter.
template <ClockSource Clock = std::chrono::steady_clock>
class SlidingWindowLog {
public:
    SlidingWindowLog(std::uint32_t limit, Nanos window, Clock clock = {})
        : limit_(detail::require_positive(limit, "limit")),
          window_(detail::require_positive(window, "window")),
          clock_(std::move(clock)) {}

    [[nodiscard]] Decision try_acquire(std::uint32_t cost = 1) {
        detail::check_cost(cost, limit_);
        std::lock_guard lock(mutex_);
        const TimePoint now = clock_.now();
        // Drop permits that have left the window (granted at or before now - window).
        while (!log_.empty() && log_.front() + window_ <= now) log_.pop_front();

        if (log_.size() + cost <= limit_) {
            log_.insert(log_.end(), cost, now);  // one entry per permit
            return {true, Nanos::zero()};
        }
        // We need `must_expire` of the oldest entries to leave the window; the last of them decides.
        const std::size_t must_expire = log_.size() + cost - limit_;
        return {false, log_[must_expire - 1] + window_ - now};
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return limit_; }

private:
    const std::uint32_t limit_;
    const Nanos window_;
    Clock clock_;
    std::mutex mutex_;  // guards log_
    std::deque<TimePoint> log_;  // oldest at the front
};

// =====================================================================================
// Sliding window counter
// =====================================================================================

/// Approximates a sliding window with two counters: the previous and the current aligned window.
/// The previous window's count is weighted by how much of it still overlaps the sliding window:
///
///   estimate = previous * (1 - fraction_of_current_window_elapsed) + current
///
/// O(1) memory like the fixed window, but without its 2x boundary burst. It assumes requests in the
/// previous window were spread evenly, so it can be slightly too strict or too lenient.
template <ClockSource Clock = std::chrono::steady_clock>
class SlidingWindowCounter {
public:
    SlidingWindowCounter(std::uint32_t limit, Nanos window, Clock clock = {})
        : limit_(detail::require_positive(limit, "limit")),
          window_(detail::require_positive(window, "window")),
          clock_(std::move(clock)),
          current_start_(detail::window_start(clock_.now(), window_)) {}

    [[nodiscard]] Decision try_acquire(std::uint32_t cost = 1) {
        detail::check_cost(cost, limit_);
        std::lock_guard lock(mutex_);
        const TimePoint now = clock_.now();
        roll(now);
        if (estimate_at(now) + cost <= limit_) {
            current_ += cost;
            return {true, Nanos::zero()};
        }

        // Denied: when would this request fit (assuming no other traffic in between)? Start from the
        // closed-form answer, then confirm it with the SAME estimate_at() used for admission, so
        // floating-point rounding can never make retry_after a nanosecond too short.
        Nanos wait = closed_form_wait(now, cost);
        for (int step = 0; step < 64 && estimate_at(now + wait) + cost > limit_; ++step) wait += Nanos(1);
        if (estimate_at(now + wait) + cost > limit_) {
            wait = current_start_ + 2 * window_ - now;  // fallback that always fits: both windows empty
        }
        return {false, wait};
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return limit_; }

private:
    void roll(TimePoint now) {
        const TimePoint start = detail::window_start(now, window_);
        if (start == current_start_) return;
        // Exactly one window later: the current window becomes the previous one.
        // Two or more windows later: both windows are stale.
        previous_ = (start - current_start_ == window_) ? current_ : 0;
        current_ = 0;
        current_start_ = start;
    }

    /// The weighted estimate a request at time `at` (>= the last roll) would see, without mutating state.
    [[nodiscard]] double estimate_at(TimePoint at) const {
        const TimePoint start = detail::window_start(at, window_);
        std::uint64_t previous = 0;
        std::uint64_t current = 0;
        if (start == current_start_) {
            previous = previous_;
            current = current_;
        } else if (start - current_start_ == window_) {
            previous = current_;  // what roll() will do once that window begins
        }
        const double fraction = static_cast<double>((at - start).count()) / static_cast<double>(window_.count());
        return static_cast<double>(previous) * (1.0 - fraction) + static_cast<double>(current);
    }

    /// Solves estimate(now + t) + cost <= limit for t.
    [[nodiscard]] Nanos closed_form_wait(TimePoint now, std::uint32_t cost) const {
        const double window = static_cast<double>(window_.count());
        if (current_ + cost <= limit_) {
            // Fits later in THIS window, once the previous window's weight has decayed (previous_ > 0 here):
            //   previous * (1 - f) + current + cost <= limit   <=>   f >= 1 - (limit - current - cost) / previous
            const double needed = 1.0 - static_cast<double>(limit_ - current_ - cost) / static_cast<double>(previous_);
            const double elapsed = static_cast<double>((now - current_start_).count());
            return std::max(Nanos(1), Nanos(static_cast<Nanos::rep>(std::ceil(needed * window - elapsed))));
        }
        // Needs the next window, where THIS window's count becomes the decaying "previous" (current_ > 0 here):
        //   current * (1 - f) + cost <= limit   <=>   f >= 1 - (limit - cost) / current
        const Nanos to_next = current_start_ + window_ - now;
        const double needed = std::max(0.0, 1.0 - static_cast<double>(limit_ - cost) / static_cast<double>(current_));
        return to_next + Nanos(static_cast<Nanos::rep>(std::ceil(needed * window)));
    }

    const std::uint32_t limit_;
    const Nanos window_;
    Clock clock_;
    std::mutex mutex_;  // guards everything below
    TimePoint current_start_;
    std::uint64_t previous_ = 0;
    std::uint64_t current_ = 0;
};

// =====================================================================================
// Per-key limiter (one limiter per client, user, API key, IP address...)
// =====================================================================================

/// Lazily creates one limiter per key with `factory`. Keys are spread over `shards` independently
/// locked hash maps so unrelated keys rarely contend on the same mutex. Each limiter is held by
/// shared_ptr: the shard lock is held only for the map lookup, never while the limiter runs, and an
/// idle-key eviction cannot free a limiter another thread is still using.
template <RateLimiter Limiter, typename Key = std::string, ClockSource Clock = std::chrono::steady_clock>
class KeyedRateLimiter {
public:
    using Factory = std::function<std::unique_ptr<Limiter>()>;

    explicit KeyedRateLimiter(Factory factory, std::size_t shards = 16, Clock clock = {})
        : factory_(std::move(factory)),
          clock_(std::move(clock)),
          shards_(std::bit_ceil(std::max<std::size_t>(shards, 1))) {}  // power of two: cheap masking

    [[nodiscard]] Decision try_acquire(const Key& key, std::uint32_t cost = 1) {
        return limiter_for(key)->try_acquire(cost);
    }

    /// Forgets keys not used for `idle_for`; returns how many were removed. Run it periodically.
    /// Choose idle_for >= the time a limiter needs to fully recover, and eviction is invisible to
    /// clients (an evicted key comes back as a fresh limiter, which it would have been anyway).
    std::size_t evict_idle(Nanos idle_for) {
        const TimePoint cutoff = clock_.now() - idle_for;
        std::size_t removed = 0;
        for (Shard& shard : shards_) {
            std::lock_guard lock(shard.mutex);
            removed += std::erase_if(shard.entries, [&](const auto& entry) { return entry.second.last_used < cutoff; });
        }
        return removed;
    }

    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const Shard& shard : shards_) {
            std::lock_guard lock(shard.mutex);
            total += shard.entries.size();
        }
        return total;
    }

private:
    struct Entry {
        std::shared_ptr<Limiter> limiter;
        TimePoint last_used{};
    };
    struct Shard {
        mutable std::mutex mutex;  // guards entries
        std::unordered_map<Key, Entry> entries;
    };

    // Fibonacci hashing: multiply by 2^64 / golden ratio and take high bits. The map inside a shard
    // also uses std::hash, so picking shards from the LOW bits would make every key in a shard share
    // those bits and cluster in the shard's buckets.
    [[nodiscard]] Shard& shard_for(const Key& key) {
        const std::uint64_t mixed = static_cast<std::uint64_t>(std::hash<Key>{}(key)) * 0x9E3779B97F4A7C15ULL;
        return shards_[static_cast<std::size_t>(mixed >> 32) & (shards_.size() - 1)];
    }

    [[nodiscard]] std::shared_ptr<Limiter> limiter_for(const Key& key) {
        Shard& shard = shard_for(key);
        const TimePoint now = clock_.now();
        std::lock_guard lock(shard.mutex);
        auto [it, inserted] = shard.entries.try_emplace(key);
        if (inserted) {
            try {
                it->second.limiter = factory_();
                if (!it->second.limiter) throw std::logic_error("rate limiter factory returned null");
            } catch (...) {
                shard.entries.erase(it);  // never leave an entry without a limiter
                throw;
            }
        }
        it->second.last_used = now;
        return it->second.limiter;  // the limiter runs after the shard lock is released
    }

    Factory factory_;
    Clock clock_;
    std::vector<Shard> shards_;  // never resized after construction (Shard is not movable)
};

}  // namespace roadmap::rate
