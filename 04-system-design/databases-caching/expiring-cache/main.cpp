// =====================================================================================================
// Concurrent cache with expiration (C++23)
// =====================================================================================================
//
// WHAT IT DOES
//   A key -> value cache shared by many threads. Every entry has a time-to-live (TTL): after its deadline
//   the entry is treated as absent and its memory is reclaimed. Features:
//     * put(key, value, ttl), get(key), erase(key), ttl_remaining(key)
//     * get_or_load(key, ttl, loader): on a miss, load the value ONCE even if many threads ask at the
//       same time ("single-flight", which prevents a cache stampede)
//     * optional capacity limit per shard, evicting the entry that expires soonest
//     * a background sweeper thread that frees expired entries nobody reads again
//
// 1) DATA RACES: how the cache stays thread-safe
//   The key space is split into N SHARDS (a power of two), each with its own mutex, hash map and expiry
//   heap. A key always maps to the same shard, so operations on different shards never contend. One
//   global lock would serialise every thread; sharding gives close to N-way parallelism.
//   Why not std::shared_mutex (readers in parallel)? Because get() is NOT read-only here: finding an
//   expired entry deletes it. And for short critical sections a plain mutex is usually faster anyway.
//   get() returns std::optional<V> BY VALUE. Returning a reference or pointer into the map would race:
//   once the shard lock is released, another thread may overwrite, erase or expire that entry, and the
//   caller would be reading freed memory. (For large values store std::shared_ptr<const V> as V, so the
//   copy is only a reference-count increment.)
//   Stats are std::atomic counters with memory_order_relaxed: independent counters that guard no data.
//
// 2) EXPIRATION: lazy + active
//   * Lazy: get() checks the deadline and treats an expired entry as a miss, deleting it on the spot.
//     This alone is always CORRECT (no stale value is ever returned), but an expired key that is never read
//     again would stay in memory forever: a leak.
//   * Active: each shard keeps a MIN-HEAP of (deadline, key, version). The sweeper thread wakes every
//     sweep_interval, pops entries whose deadline has passed, and deletes them. It processes at most
//     sweep_batch heap items per shard per pass, so it never holds a shard lock for long.
//   Redis uses the same pair: lazy expiry on access plus a periodic active expiry cycle.
//
//   VERSIONED HEAP ENTRIES (lazy deletion): overwriting a key with a new TTL does not search the heap for
//   the old item (that would be O(n)). Instead every write bumps a per-shard version. A heap item is acted
//   on only if its version still matches the live entry; stale items are discarded when popped. When stale
//   items pile up (heap > 2 * entries), the heap is rebuilt from the map in O(n), which is amortised O(1).
//
// 3) EVICTION when a capacity is set
//   Inserting a NEW key into a full shard first removes already-expired entries, then evicts the live
//   entry with the EARLIEST deadline: the heap top. That is like Redis "volatile-ttl", and the heap
//   makes it O(log n). (An LRU policy would need a recency list as in the LRU-cache exercise.)
//
// 4) TIME HANDLING
//   * std::chrono::steady_clock, never system_clock. Wall-clock time can jump backwards or forwards
//     (NTP corrections, manual changes, DST), which would expire everything at once or nothing for hours.
//     steady_clock only moves forward.
//   * The deadline is computed ONCE at insertion (now + ttl), clamped so a huge TTL cannot overflow.
//   * An entry is expired when now >= deadline. The clock is read UNDER the shard lock, so decisions in
//     one shard are made in time order.
//   * The clock is a template parameter (ClockSource), so the demo can use a ManualClock and control
//     time exactly instead of sleeping.
//   * TTL JITTER: if 10,000 keys are filled at startup with the same TTL, they all expire in the same
//     instant and every request misses at once. ttl_jitter spreads deadlines randomly (e.g. ±10%).
//
// 5) CACHE STAMPEDE and get_or_load
//   When a hot key expires, hundreds of threads miss at the same moment and all hit the database together.
//   get_or_load keeps a per-shard map of in-flight loads (key -> std::shared_future<V>). The first thread
//   to miss becomes the LEADER: it registers a promise, runs the loader OUTSIDE the lock, stores the
//   result, and fulfils the promise. Every other thread that misses meanwhile finds the in-flight entry
//   and waits on the same shared_future. If the loader throws, the exception is delivered to every waiter
//   and nothing is cached. Warning: a loader must not call get_or_load for the SAME key, because it would
//   wait for its own result forever.
//
// 6) LIFETIME
//   The sweeper is a std::jthread declared as the LAST member, so it is stopped and joined before the
//   shards are destroyed. It sleeps in condition_variable_any::wait_for(..., stop_token, ...), so the
//   destructor wakes it immediately instead of waiting out the sweep interval.
// =====================================================================================================

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace roadmap::cache {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::nanoseconds;

template <typename C>
concept ClockSource = requires(const C& clock) {
    { clock.now() } -> std::same_as<TimePoint>;
};

/// A clock that moves only when told to. Copies share the same time. Thread-safe.
class ManualClock {
public:
    [[nodiscard]] TimePoint now() const noexcept {
        return TimePoint(std::chrono::duration_cast<TimePoint::duration>(Duration(ticks_->load(std::memory_order_relaxed))));
    }
    void advance(Duration step) noexcept { ticks_->fetch_add(step.count(), std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<Duration::rep>> ticks_ = std::make_shared<std::atomic<Duration::rep>>(0);
};

template <typename K, typename V, ClockSource Clock = std::chrono::steady_clock, typename Hash = std::hash<K>>
class ExpiringCache {
public:
    struct Options {
        std::size_t shards = 16;                        // rounded up to a power of two
        std::size_t max_entries_per_shard = 0;          // 0 = unbounded
        std::chrono::milliseconds sweep_interval{100};  // 0 = no background sweeper (lazy expiry only)
        std::size_t sweep_batch = 1024;                 // max heap items examined per shard per pass
        double ttl_jitter = 0.0;                        // e.g. 0.1 = each TTL randomly shortened by up to 10%
    };

    struct Stats {
        std::uint64_t hits = 0, misses = 0, expired_on_read = 0, expired_by_sweep = 0, evicted = 0, loads = 0,
                      coalesced = 0;
    };

    explicit ExpiringCache(Options options = {}, Clock clock = {})
        : options_(options), clock_(std::move(clock)), shards_(std::bit_ceil(std::max<std::size_t>(options.shards, 1))) {
        if (options_.sweep_interval > 0ms) {
            sweeper_ = std::jthread([this](std::stop_token stop) { sweep_loop(std::move(stop)); });
        }
    }

    ExpiringCache(const ExpiringCache&) = delete;
    ExpiringCache& operator=(const ExpiringCache&) = delete;

    /// Inserts or overwrites `key`. A non-positive TTL means "already expired": the key is removed.
    void put(const K& key, V value, Duration ttl) {
        if (ttl <= Duration::zero()) {
            erase(key);
            return;
        }
        Shard& shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        const TimePoint now = clock_.now();
        insert_locked(shard, key, std::move(value), deadline(now, jittered(ttl)), now);
    }

    /// The value, or nullopt if the key is absent or expired. Returns a COPY (see note 1 above).
    [[nodiscard]] std::optional<V> get(const K& key) {
        Shard& shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (it->second.expires_at <= clock_.now()) {  // lazy expiry: never return a stale value
            shard.entries.erase(it);                  // its heap item becomes stale and is skipped later
            expired_on_read_.fetch_add(1, std::memory_order_relaxed);
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second.value;
    }

    /// Returns the cached value, or loads it with `loader(key)` exactly once across concurrent callers.
    template <typename Loader>
        requires std::invocable<Loader&, const K&> && std::convertible_to<std::invoke_result_t<Loader&, const K&>, V>
    V get_or_load(const K& key, Duration ttl, Loader loader) {
        Shard& shard = shard_for(key);
        std::promise<V> promise;
        std::shared_future<V> pending;
        {
            std::lock_guard lock(shard.mutex);
            const TimePoint now = clock_.now();
            if (auto it = shard.entries.find(key); it != shard.entries.end() && it->second.expires_at > now) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second.value;
            }
            misses_.fetch_add(1, std::memory_order_relaxed);
            if (auto flight = shard.inflight.find(key); flight != shard.inflight.end()) {
                pending = flight->second;  // someone else is already loading this key: wait for it
            } else {
                shard.inflight.emplace(key, promise.get_future().share());  // we are the leader
            }
        }
        if (pending.valid()) {
            coalesced_.fetch_add(1, std::memory_order_relaxed);
            return pending.get();  // rethrows the leader's exception, if any
        }

        try {
            V value = std::invoke(loader, key);  // the slow part runs WITHOUT holding the shard lock
            loads_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(shard.mutex);
                const TimePoint now = clock_.now();
                insert_locked(shard, key, value, deadline(now, jittered(ttl)), now);
                shard.inflight.erase(key);  // in the same critical section, so a newcomer either waits or hits
            }
            promise.set_value(value);
            return value;
        } catch (...) {
            {
                std::lock_guard lock(shard.mutex);
                shard.inflight.erase(key);  // nothing cached: the next caller retries the load
            }
            promise.set_exception(std::current_exception());
            throw;
        }
    }

    bool erase(const K& key) {
        Shard& shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        return shard.entries.erase(key) > 0;
    }

    /// Time left before `key` expires (nullopt if absent or already expired). Does not change stats.
    [[nodiscard]] std::optional<Duration> ttl_remaining(const K& key) const {
        const Shard& shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        auto it = shard.entries.find(key);
        const TimePoint now = clock_.now();
        if (it == shard.entries.end() || it->second.expires_at <= now) return std::nullopt;
        return std::chrono::duration_cast<Duration>(it->second.expires_at - now);
    }

    /// Entries physically stored (including expired ones not yet reclaimed).
    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const Shard& shard : shards_) {
            std::lock_guard lock(shard.mutex);
            total += shard.entries.size();
        }
        return total;
    }

    /// One active-expiry pass over every shard (the sweeper calls this). Returns entries removed.
    std::size_t purge_expired() {
        std::size_t removed = 0;
        for (Shard& shard : shards_) {
            std::lock_guard lock(shard.mutex);
            removed += purge_expired_locked(shard, clock_.now(), options_.sweep_batch);
        }
        expired_by_sweep_.fetch_add(removed, std::memory_order_relaxed);
        return removed;
    }

    [[nodiscard]] Stats stats() const {
        const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
        return {r(hits_), r(misses_), r(expired_on_read_), r(expired_by_sweep_), r(evicted_), r(loads_), r(coalesced_)};
    }

private:
    struct Entry {
        V value;
        TimePoint expires_at;
        std::uint64_t version;
    };
    struct HeapItem {
        TimePoint expires_at;
        K key;
        std::uint64_t version;
    };
    struct Shard {
        mutable std::mutex mutex;  // guards everything below
        std::unordered_map<K, Entry, Hash> entries;
        std::vector<HeapItem> heap;  // min-heap on expires_at (std::push_heap / pop_heap with `later`)
        std::unordered_map<K, std::shared_future<V>, Hash> inflight;
        std::uint64_t next_version = 0;
    };

    static bool later(const HeapItem& a, const HeapItem& b) noexcept { return a.expires_at > b.expires_at; }

    [[nodiscard]] static TimePoint deadline(TimePoint now, Duration ttl) noexcept {
        if (ttl >= TimePoint::max() - now) return TimePoint::max();  // clamp instead of overflowing
        return now + std::chrono::duration_cast<TimePoint::duration>(ttl);
    }

    [[nodiscard]] Duration jittered(Duration ttl) const {
        if (options_.ttl_jitter <= 0.0) return ttl;
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_real_distribution<double> shrink(0.0, std::min(options_.ttl_jitter, 1.0));
        return std::max(Duration(1), Duration(static_cast<Duration::rep>(static_cast<double>(ttl.count()) * (1.0 - shrink(rng)))));
    }

    // Fibonacci hashing: take HIGH bits of hash * 2^64/phi, so the shard choice does not reuse the
    // same low bits the shard's own unordered_map uses for its buckets.
    [[nodiscard]] Shard& shard_for(const K& key) {
        const std::uint64_t mixed = static_cast<std::uint64_t>(Hash{}(key)) * 0x9E3779B97F4A7C15ULL;
        return shards_[static_cast<std::size_t>(mixed >> 32) & (shards_.size() - 1)];
    }
    [[nodiscard]] const Shard& shard_for(const K& key) const { return const_cast<ExpiringCache*>(this)->shard_for(key); }

    void insert_locked(Shard& shard, const K& key, V value, TimePoint expires_at, TimePoint now) {
        auto it = shard.entries.find(key);
        if (it == shard.entries.end() && options_.max_entries_per_shard != 0 &&
            shard.entries.size() >= options_.max_entries_per_shard) {
            make_room_locked(shard, now);
        }
        const std::uint64_t version = ++shard.next_version;
        shard.heap.push_back({expires_at, key, version});
        std::ranges::push_heap(shard.heap, later);
        if (it != shard.entries.end()) {
            it->second = Entry{std::move(value), expires_at, version};  // old heap item is now stale
        } else {
            shard.entries.emplace(key, Entry{std::move(value), expires_at, version});
        }
        if (shard.heap.size() > 2 * shard.entries.size() + 64) rebuild_heap_locked(shard);
    }

    // Pops the earliest heap item (moving its key out, which std::priority_queue would not allow).
    static HeapItem pop_earliest(Shard& shard) {
        std::ranges::pop_heap(shard.heap, later);
        HeapItem item = std::move(shard.heap.back());
        shard.heap.pop_back();
        return item;
    }

    static bool is_current(const Shard& shard, const HeapItem& item) {
        auto it = shard.entries.find(item.key);
        return it != shard.entries.end() && it->second.version == item.version;
    }

    std::size_t purge_expired_locked(Shard& shard, TimePoint now, std::size_t budget) {
        std::size_t removed = 0;
        for (std::size_t examined = 0; examined < budget && !shard.heap.empty() && shard.heap.front().expires_at <= now;
             ++examined) {
            HeapItem item = pop_earliest(shard);
            if (is_current(shard, item)) {  // stale items (overwritten or erased keys) are just dropped
                shard.entries.erase(item.key);
                ++removed;
            }
        }
        return removed;
    }

    void make_room_locked(Shard& shard, TimePoint now) {
        expired_by_sweep_.fetch_add(purge_expired_locked(shard, now, std::numeric_limits<std::size_t>::max()),
                                    std::memory_order_relaxed);
        // Every live entry has exactly one current heap item, so the heap cannot run dry while entries remain.
        while (shard.entries.size() >= options_.max_entries_per_shard && !shard.heap.empty()) {
            HeapItem item = pop_earliest(shard);
            if (!is_current(shard, item)) continue;
            shard.entries.erase(item.key);  // evict the entry that would have expired soonest
            evicted_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void rebuild_heap_locked(Shard& shard) {
        shard.heap.clear();
        shard.heap.reserve(shard.entries.size());
        for (const auto& [key, entry] : shard.entries) shard.heap.push_back({entry.expires_at, key, entry.version});
        std::ranges::make_heap(shard.heap, later);
    }

    void sweep_loop(std::stop_token stop) {
        std::mutex sleep_mutex;
        std::condition_variable_any sleeper;
        std::unique_lock lock(sleep_mutex);
        while (!stop.stop_requested()) {
            // Sleeps for sweep_interval, but returns at once when the destructor requests a stop.
            sleeper.wait_for(lock, stop, options_.sweep_interval, [] { return false; });
            if (stop.stop_requested()) return;
            purge_expired();
        }
    }

    const Options options_;
    Clock clock_;
    std::vector<Shard> shards_;  // sized once in the constructor; Shard is not movable
    std::atomic<std::uint64_t> hits_{0}, misses_{0}, expired_on_read_{0}, expired_by_sweep_{0}, evicted_{0}, loads_{0},
        coalesced_{0};
    std::jthread sweeper_;  // LAST member: stopped and joined before anything above is destroyed
};

}  // namespace roadmap::cache

// =====================================================================================================
// Demo
// =====================================================================================================

namespace {

using namespace roadmap::cache;

std::string show_ttl(const std::optional<Duration>& ttl) {
    return ttl ? std::format("{} ms left", std::chrono::duration_cast<std::chrono::milliseconds>(*ttl).count())
               : std::string("absent/expired");
}

}  // namespace

int main() {
    // ------------------------------------------------------------------ 1. TTL basics (manual clock)
    std::println("1) TTL basics with a manual clock (no sleeping, exact timing)");
    {
        ManualClock clock;
        ExpiringCache<std::string, std::string, ManualClock> sessions({.sweep_interval = 0ms}, clock);
        sessions.put("alice", "token-a", 1s);
        sessions.put("bob", "token-b", 3s);
        clock.advance(2s);
        std::println("   t=2s  alice: {:<15} bob: {}", sessions.get("alice").value_or("<miss>"), sessions.get("bob").value_or("<miss>"));
        std::println("         ttl_remaining(bob) = {}", show_ttl(sessions.ttl_remaining("bob")));
        sessions.put("bob", "token-b2", 5s);  // overwrite = new value AND new deadline
        clock.advance(2s);
        std::println("   t=4s  bob after refresh: {} ({})", sessions.get("bob").value_or("<miss>"), show_ttl(sessions.ttl_remaining("bob")));
        const auto s = sessions.stats();
        std::println("   stats: hits {} misses {} expired_on_read {}", s.hits, s.misses, s.expired_on_read);
    }

    // ------------------------------------------------------------------ 2. lazy vs active expiry
    std::println("\n2) 20,000 entries with a 100 ms TTL that are NEVER read again: lazy-only vs background sweeper");
    {
        ExpiringCache<int, int> lazy_only({.sweep_interval = 0ms});
        ExpiringCache<int, int> swept({.sweep_interval = 20ms, .sweep_batch = 4096});
        for (int i = 0; i < 20'000; ++i) {
            lazy_only.put(i, i, 100ms);
            swept.put(i, i, 100ms);
        }
        const auto start = std::chrono::steady_clock::now();
        for (auto at : {0ms, 50ms, 150ms, 250ms}) {
            std::this_thread::sleep_until(start + at);
            std::println("   t={:>3} ms   stored entries: lazy-only {:>6}   with sweeper {:>6}", at.count(), lazy_only.size(), swept.size());
        }
        std::println("   lazy-only keeps expired data in memory until someone reads it; the sweeper reclaimed {} entries",
                     swept.stats().expired_by_sweep);
    }

    // ------------------------------------------------------------------ 3. concurrency
    std::println("\n3) 8 threads, 400 ms of random get/put/erase on 1,000 keys, TTLs of 1-20 ms expiring concurrently");
    {
        ExpiringCache<int, int> cache({.shards = 16, .sweep_interval = 5ms});
        std::atomic<std::uint64_t> ops{0}, corrupted{0};
        std::latch start(8);
        {
            std::vector<std::jthread> workers;
            for (int t = 0; t < 8; ++t) {
                workers.emplace_back([&, t](std::stop_token stop) {
                    std::mt19937 rng(static_cast<unsigned>(t));
                    std::uniform_int_distribution<int> key(0, 999), op(0, 9), ttl_ms(1, 20);
                    start.arrive_and_wait();
                    while (!stop.stop_requested()) {
                        const int k = key(rng);
                        const int o = op(rng);
                        if (o < 6) {
                            // Invariant: a value for key k is always k * 1000 + writer id. A torn or misplaced
                            // value (what a data race could produce) would break it.
                            if (auto v = cache.get(k); v && *v / 1000 != k) corrupted.fetch_add(1);
                        } else if (o < 9) {
                            cache.put(k, k * 1000 + t, std::chrono::milliseconds(ttl_ms(rng)));
                        } else {
                            cache.erase(k);
                        }
                        ops.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            std::this_thread::sleep_for(400ms);
        }  // jthreads: request_stop + join
        const auto s = cache.stats();
        std::println("   {:.1f} M operations ({:.1f} M ops/s), corrupted values: {}", static_cast<double>(ops) / 1e6,
                     static_cast<double>(ops) / 1e6 / 0.4, corrupted.load());
        std::println("   hits {}  misses {}  expired_on_read {}  expired_by_sweep {}  still stored {}", s.hits, s.misses,
                     s.expired_on_read, s.expired_by_sweep, cache.size());
    }

    // ------------------------------------------------------------------ 4. stampede protection
    std::println("\n4) Cache stampede: 32 threads request the same missing key at once; the loader takes 100 ms");
    {
        auto slow_database = [](std::atomic<int>& calls) {
            return [&calls](const std::string& key) {
                calls.fetch_add(1);
                std::this_thread::sleep_for(100ms);
                return "value-of-" + key;
            };
        };
        auto run = [&](const char* name, auto&& request) {
            ExpiringCache<std::string, std::string> cache;
            std::atomic<int> loads{0};
            std::atomic<int> correct{0};
            std::latch start(32);
            const auto t0 = std::chrono::steady_clock::now();
            {
                std::vector<std::jthread> clients;
                for (int i = 0; i < 32; ++i) {
                    clients.emplace_back([&] {
                        start.arrive_and_wait();
                        if (request(cache, slow_database(loads)) == "value-of-config") ++correct;
                    });
                }
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            std::println("   {:<32} database calls {:>2}   correct answers {}/32   {} ms", name, loads.load(), correct.load(), ms.count());
        };
        run("naive get() -> load -> put()", [](auto& cache, auto loader) {
            if (auto v = cache.get("config")) return *v;
            std::string v = loader("config");
            cache.put("config", v, 10s);
            return v;
        });
        run("get_or_load() (single-flight)", [](auto& cache, auto loader) { return cache.get_or_load("config", 10s, loader); });
    }

    // ------------------------------------------------------------------ 5. capacity eviction
    std::println("\n5) Capacity 3 (one shard): inserting a 4th key evicts the entry that expires soonest");
    {
        ManualClock clock;
        ExpiringCache<std::string, int, ManualClock> cache({.shards = 1, .max_entries_per_shard = 3, .sweep_interval = 0ms}, clock);
        cache.put("a", 1, 30s);
        cache.put("b", 2, 10s);
        cache.put("c", 3, 20s);
        cache.put("d", 4, 15s);
        for (const char* key : {"a", "b", "c", "d"}) std::println("   {}: {}", key, show_ttl(cache.ttl_remaining(key)));
        std::println("   evicted: {} (b had the earliest deadline)", cache.stats().evicted);
    }

    // ------------------------------------------------------------------ 6. jitter
    std::println("\n6) TTL jitter: 1,000 keys written together with TTL 10 s and ttl_jitter = 0.2");
    {
        ManualClock clock;
        ExpiringCache<int, int, ManualClock> cache({.sweep_interval = 0ms, .ttl_jitter = 0.2}, clock);
        for (int i = 0; i < 1000; ++i) cache.put(i, i, 10s);
        Duration lo = Duration::max(), hi = Duration::zero();
        for (int i = 0; i < 1000; ++i) {
            const Duration d = *cache.ttl_remaining(i);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        std::println("   deadlines spread between {:.2f} s and {:.2f} s, so the keys do not all expire in the same instant",
                     std::chrono::duration<double>(lo).count(), std::chrono::duration<double>(hi).count());
    }
}
