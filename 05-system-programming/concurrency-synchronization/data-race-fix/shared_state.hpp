#pragma once
// Data races in shared state: three buggy classes and their fixes.
//
//   Case 1  Metrics      ++counter from many threads        -> lost updates          fix: atomics or mutex
//   Case 2  Bank         two balances that must sum to N   -> broken invariant      fix: one mutex for both
//   Case 3  Publication  "write data, then set a flag"     -> stale/partial reads   fix: release/acquire
//
// The Unsafe* classes are DELIBERATELY broken. Calling them from several threads is undefined
// behaviour; they exist only so the demo can show the symptoms. See README.md.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace roadmap::race {

// =====================================================================================
// Case 1: request metrics
// =====================================================================================

struct MetricsSnapshot {
    std::uint64_t requests = 0;
    std::uint64_t total_latency = 0;
    std::uint64_t max_latency = 0;

    [[nodiscard]] double average_latency() const noexcept {
        return requests == 0 ? 0.0 : static_cast<double>(total_latency) / static_cast<double>(requests);
    }
};

/// BUGGY. Correct with one thread, wrong with several.
class UnsafeMetrics {
public:
    void record(std::uint64_t latency) noexcept {
        ++requests_;               // BUG 1: load, add, store. Two threads can load the same value,
        total_latency_ += latency; //        both store value+1, and one increment is lost.
        if (latency > max_latency_) {  // BUG 2: check-then-act. Another thread can store a bigger
            max_latency_ = latency;    //        max between our check and our store; we overwrite it.
        }
    }

    // BUG 3: the three fields are read at different moments, so the snapshot can mix
    //        "before" and "after" values of a concurrent record() (a torn read).
    [[nodiscard]] MetricsSnapshot snapshot() const noexcept {
        return {requests_, total_latency_, max_latency_};
    }

private:
    std::uint64_t requests_ = 0;
    std::uint64_t total_latency_ = 0;
    std::uint64_t max_latency_ = 0;
};

/// FIX A: one atomic per field. Fast, lock-free. Each field is exact, but a snapshot is NOT a
/// consistent picture of all three at one instant (see LockedMetrics if you need that).
class AtomicMetrics {
public:
    void record(std::uint64_t latency) noexcept {
        // Atomic read-modify-write: no increment can be lost.
        // memory_order_relaxed is enough: these counters do not guard any other data.
        requests_.fetch_add(1, std::memory_order_relaxed);
        total_latency_.fetch_add(latency, std::memory_order_relaxed);
        update_max(latency);
    }

    [[nodiscard]] MetricsSnapshot snapshot() const noexcept {
        return {requests_.load(std::memory_order_relaxed), total_latency_.load(std::memory_order_relaxed),
                max_latency_.load(std::memory_order_relaxed)};
    }

private:
    // There is no atomic "fetch_max" before C++26, so use a compare-and-swap (CAS) loop:
    // "if max is still `current`, replace it with `latency`; otherwise reload and retry".
    void update_max(std::uint64_t latency) noexcept {
        std::uint64_t current = max_latency_.load(std::memory_order_relaxed);
        while (latency > current &&
               !max_latency_.compare_exchange_weak(current, latency, std::memory_order_relaxed)) {
            // compare_exchange_weak failed: `current` now holds the latest value; the loop re-checks.
        }
    }

    // Each counter on its own cache line: without this, threads updating different counters keep
    // stealing the same 64-byte line from each other ("false sharing"), which is slow but correct.
    static constexpr std::size_t kCacheLine = 64;
    alignas(kCacheLine) std::atomic<std::uint64_t> requests_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> total_latency_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> max_latency_{0};
};

/// FIX B: one mutex for all fields. Simple, and snapshots are always internally consistent.
class LockedMetrics {
public:
    void record(std::uint64_t latency) {
        std::lock_guard lock(mutex_);
        ++state_.requests;
        state_.total_latency += latency;
        state_.max_latency = std::max(state_.max_latency, latency);
    }

    [[nodiscard]] MetricsSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

private:
    mutable std::mutex mutex_;  // guards state_
    MetricsSnapshot state_;
};

// =====================================================================================
// Case 2: a bank that must never create or destroy money
// =====================================================================================

/// STILL BUGGY, even though it has NO data race: every access is atomic.
/// Atomic operations are indivisible one at a time, but a transfer is TWO of them.
class AtomicFieldBank {
public:
    AtomicFieldBank(std::size_t accounts, std::int64_t initial_balance) : balances_(accounts) {
        for (auto& balance : balances_) balance.store(initial_balance);
    }

    bool transfer(std::size_t from, std::size_t to, std::int64_t amount) {
        if (balances_[from].load() < amount) return false;  // BUG 1: check-then-act, an overdraft is possible
        balances_[from].fetch_sub(amount);
        // BUG 2: right here the money has left `from` but not reached `to`.
        // An auditor running now sees a total that is too small.
        balances_[to].fetch_add(amount);
        return true;
    }

    [[nodiscard]] std::int64_t total() const {
        std::int64_t sum = 0;
        for (const auto& balance : balances_) sum += balance.load();
        return sum;
    }

    [[nodiscard]] std::int64_t balance(std::size_t account) const { return balances_.at(account).load(); }
    [[nodiscard]] std::size_t accounts() const noexcept { return balances_.size(); }

private:
    std::vector<std::atomic<std::int64_t>> balances_;
};

/// FIX: every operation that reads or writes more than one balance holds the same mutex,
/// so nobody can observe a transfer half-done, and the overdraft check cannot go stale.
class LockedBank {
public:
    LockedBank(std::size_t accounts, std::int64_t initial_balance) : balances_(accounts, initial_balance) {}

    /// Returns false if `from` has insufficient funds. Throws on invalid arguments.
    bool transfer(std::size_t from, std::size_t to, std::int64_t amount) {
        if (amount <= 0) throw std::invalid_argument("transfer amount must be positive");
        std::lock_guard lock(mutex_);
        if (from >= balances_.size() || to >= balances_.size()) throw std::out_of_range("no such account");
        if (from == to || balances_[from] < amount) return false;  // check and act under ONE lock
        balances_[from] -= amount;
        balances_[to] += amount;
        return true;
    }

    [[nodiscard]] std::int64_t total() const {
        std::lock_guard lock(mutex_);
        return std::reduce(balances_.begin(), balances_.end(), std::int64_t{0});
    }

    [[nodiscard]] std::vector<std::int64_t> balances() const {
        std::lock_guard lock(mutex_);
        return balances_;
    }

    [[nodiscard]] std::size_t accounts() const noexcept { return balances_.size(); }  // never changes

private:
    mutable std::mutex mutex_;  // guards balances_
    std::vector<std::int64_t> balances_;
};

// =====================================================================================
// Case 3: publishing a value to other threads
// =====================================================================================
//
// BUGGY version (not compiled; it can hang or read garbage):
//
//     std::string config;  bool ready = false;
//     writer:  config = load();  ready = true;
//     reader:  while (!ready) {}  use(config);
//
//   * `ready` is a plain bool: the compiler may hoist the load out of the loop -> infinite loop.
//   * Nothing orders the two writes: the reader may see ready == true but a half-written config.

/// FIX: a release store paired with an acquire load creates a "happens-before" edge:
/// everything the writer did before the release is visible to a reader whose acquire sees it.
template <typename T>
class OneShotPublisher {
public:
    /// Publishes exactly once. A second call throws std::logic_error.
    void publish(T value) {
        if (claimed_.test_and_set(std::memory_order_relaxed)) {
            throw std::logic_error("OneShotPublisher::publish called twice");
        }
        value_.emplace(std::move(value));                // (1) ordinary write
        ready_.store(true, std::memory_order_release);   // (2) "publish": (1) happens-before any acquire that sees true
        ready_.notify_all();                             // C++20: wake threads blocked in wait()
    }

    /// Blocks until the value is published, then returns it. No busy-waiting.
    [[nodiscard]] const T& wait() const {
        ready_.wait(false, std::memory_order_acquire);   // sleeps while ready_ == false
        return *value_;                                  // safe: the acquire synchronised with (2)
    }

    [[nodiscard]] bool is_published() const noexcept { return ready_.load(std::memory_order_acquire); }

private:
    std::optional<T> value_;
    std::atomic<bool> ready_{false};
    std::atomic_flag claimed_;  // C++20: default-initialised to clear
};

}  // namespace roadmap::race
