// =====================================================================================================
// Diagnose a deadlock in a multithreaded program (C++23)
// =====================================================================================================
//
// THE BUG: "ABBA" lock ordering
//   transfer(from, to) locks `from`, then `to`. Two tellers run at the same time:
//     teller-1: transfer(A, B)  ->  locks A ... then waits for B
//     teller-2: transfer(B, A)  ->  locks B ... then waits for A
//   Each thread holds the lock the other one needs. Neither can continue: a DEADLOCK. The program does not
//   crash and logs nothing. It just stops making progress, which is why deadlocks are hard to diagnose.
//
// THE FOUR (COFFMAN) CONDITIONS: a deadlock needs ALL of them
//   1. Mutual exclusion   a lock is held by one thread at a time
//   2. Hold and wait      a thread holds one lock while waiting for another
//   3. No preemption      a lock cannot be taken away from its owner
//   4. Circular wait      T1 waits for T2, which waits for ... T1
//   Every fix breaks at least one of them. Lock ordering breaks (4); try_lock + back-off breaks (2).
//
// DIAGNOSIS TOOL 1: the WAIT-FOR GRAPH (finds deadlocks that HAVE happened)
//   Nodes are threads. "T1 -> T2" means T1 waits for a lock that T2 holds. A deadlock is a CYCLE.
//   TrackedMutex records, in a global LockRegistry, who OWNS each mutex and which mutex each thread is
//   WAITING for. A watchdog thread checks for cycles every few milliseconds. When it finds one, it prints
//   the cycle and picks a VICTIM: that thread's lock() throws DeadlockError, RAII releases what it held,
//   the cycle is broken, and the caller retries. This is exactly what databases do with deadlocked
//   transactions (abort one, retry).
//   (TrackedMutex waits with timed try_lock_for slices instead of blocking forever, so a victim can be
//   told to give up.)
//
// DIAGNOSIS TOOL 2: the LOCK-ORDER GRAPH, "lockdep" (finds deadlocks that COULD happen)
//   Nodes are locks. Each time a thread acquires B while already holding A, record the edge "A -> B".
//   If the graph ever gets a cycle (A -> B here, B -> A elsewhere), there is a POTENTIAL deadlock, even if
//   the timing never actually lined up. That is much stronger than waiting for a hang: the demo runs the
//   two tellers one after the other (no deadlock is possible) and lockdep still reports the bug.
//   The Linux kernel's lockdep and ThreadSanitizer's "lock-order-inversion" report work this way.
//   Rule: try_lock() acquisitions add NO edges. A try_lock never blocks, so it cannot be part of a
//   deadlock. That is also why std::scoped_lock below (which uses try_lock internally) is not flagged.
//
// THE FIXES
//   Fix 1, global lock ordering: every thread locks accounts in the same order (lowest id first). A
//          circular wait can no longer form. Works with any number of locks, if everyone follows the rule.
//   Fix 2, std::scoped_lock(a, b): C++17 locks several mutexes with a deadlock-avoidance algorithm (lock
//          one, try_lock the rest, and back off on failure). Simplest when all locks are taken in one place.
//   Fix 3, design: avoid holding two locks at all (e.g. one lock for the whole ledger, or message passing).
//
// DIAGNOSING A REAL HANG IN PRACTICE (no instrumentation)
//   Run `./deadlock_diagnosis hang`, which deadlocks for real with plain std::mutex, then in another terminal:
//     macOS:  sample <pid> 1           (or: lldb -p <pid>, then `bt all`)
//     Linux:  gdb -p <pid> -batch -ex "thread apply all bt"     (or: eu-stack -p <pid>)
//   Look for two or more threads parked in a mutex wait (__psynch_mutexwait / futex_wait), each inside
//   code that already holds another lock. Their stacks show which locks they hold and which they want.
//   CPU usage near 0% plus no progress points to a deadlock; 100% CPU plus no progress points to a livelock.
//
// PREVENTION RULES
//   * Document and enforce a lock hierarchy. Never call unknown code (callbacks, virtual functions,
//     logging that takes locks) while holding a lock.
//   * Keep critical sections short. Prefer one coarse lock over many fine ones until profiling shows otherwise.
//   * Test with lockdep-style tools (TSan) so ordering bugs are found without waiting for an unlucky timing.
// =====================================================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <functional>
#include <latch>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid() for hang mode

using namespace std::chrono_literals;

namespace roadmap::deadlock {

class TrackedMutex;

struct DeadlockError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ----------------------------------------------------------------------------------- LockRegistry
// Global bookkeeping for every TrackedMutex. Invariants (all under m_):
//   owner_[m] = T     T has acquired m and not yet released it (recorded after acquire, erased before unlock)
//   waiting_[T] = m   T is currently trying to acquire m
// So a cycle in the wait-for graph is always a REAL deadlock: no false positives.
class LockRegistry {
public:
    void name_current_thread(std::string name) {
        std::lock_guard lock(m_);
        names_[std::this_thread::get_id()] = std::move(name);
    }

    void begin_wait(const TrackedMutex* mutex) {
        std::lock_guard lock(m_);
        waiting_[std::this_thread::get_id()] = mutex;
    }

    void acquired(const TrackedMutex* mutex, bool via_try_lock);

    void released(const TrackedMutex* mutex) {
        std::lock_guard lock(m_);
        owner_.erase(mutex);
        auto& held = held_[std::this_thread::get_id()];
        if (auto it = std::ranges::find(held, mutex); it != held.end()) held.erase(it);
    }

    /// Called by a waiting thread between attempts: has the watchdog chosen it as the victim?
    bool is_victim() {
        std::lock_guard lock(m_);
        const auto self = std::this_thread::get_id();
        if (!victims_.erase(self)) return false;
        waiting_.erase(self);
        return true;
    }

    /// Watchdog: find a cycle in the wait-for graph, choose a victim, describe the cycle.
    std::optional<std::string> detect_and_break_cycle();

    std::vector<std::string> inversions() const {
        std::lock_guard lock(m_);
        return inversions_;
    }

    void reset_lock_order() {
        std::lock_guard lock(m_);
        edges_.clear();
        inversions_.clear();
    }

private:
    bool reachable(const TrackedMutex* from, const TrackedMutex* to) const;  // in the lock-order graph

    std::string name_of(std::thread::id id) const {
        auto it = names_.find(id);
        return it == names_.end() ? std::string("thread-?") : it->second;
    }

    mutable std::mutex m_;
    std::unordered_map<std::thread::id, std::string> names_;
    std::unordered_map<const TrackedMutex*, std::thread::id> owner_;
    std::unordered_map<std::thread::id, const TrackedMutex*> waiting_;
    std::unordered_map<std::thread::id, std::vector<const TrackedMutex*>> held_;
    std::set<std::pair<const TrackedMutex*, const TrackedMutex*>> edges_;  // (held, then acquired)
    std::vector<std::string> inversions_;
    std::unordered_set<std::thread::id> victims_;
};

inline LockRegistry& registry() {
    static LockRegistry instance;
    return instance;
}

// ----------------------------------------------------------------------------------- TrackedMutex
// Satisfies the standard Lockable requirements (lock / try_lock / unlock), so it works with
// std::lock_guard, std::unique_lock and std::scoped_lock.
class TrackedMutex {
public:
    explicit TrackedMutex(std::string name) : name_(std::move(name)) {}
    TrackedMutex(const TrackedMutex&) = delete;
    TrackedMutex& operator=(const TrackedMutex&) = delete;

    void lock() {
        registry().begin_wait(this);
        for (;;) {
            if (mutex_.try_lock_for(2ms)) {
                registry().acquired(this, /*via_try_lock=*/false);
                return;
            }
            if (registry().is_victim()) {
                throw DeadlockError(std::format("chosen as deadlock victim while waiting for {}", name_));
            }
        }
    }

    bool try_lock() {
        if (!mutex_.try_lock()) return false;
        registry().acquired(this, /*via_try_lock=*/true);
        return true;
    }

    void unlock() {
        registry().released(this);  // bookkeeping BEFORE the real unlock keeps owner_ truthful
        mutex_.unlock();
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
    std::timed_mutex mutex_;
};

inline void LockRegistry::acquired(const TrackedMutex* mutex, bool via_try_lock) {
    std::lock_guard lock(m_);
    const auto self = std::this_thread::get_id();
    waiting_.erase(self);
    victims_.erase(self);
    owner_[mutex] = self;
    auto& held = held_[self];
    if (!via_try_lock) {
        for (const TrackedMutex* h : held) {
            // Acquiring `mutex` while holding `h` adds the edge h -> mutex. If mutex already leads back to h,
            // this edge closes a cycle: some interleaving of the threads can deadlock.
            if (edges_.insert({h, mutex}).second && reachable(mutex, h)) {
                inversions_.push_back(std::format("{} locks {} then {}, but elsewhere {} is locked before {}",
                                                  name_of(self), h->name(), mutex->name(), mutex->name(), h->name()));
            }
        }
    }
    held.push_back(mutex);
}

inline bool LockRegistry::reachable(const TrackedMutex* from, const TrackedMutex* to) const {
    std::vector<const TrackedMutex*> stack{from};
    std::unordered_set<const TrackedMutex*> seen{from};
    while (!stack.empty()) {
        const TrackedMutex* node = stack.back();
        stack.pop_back();
        for (auto it = edges_.lower_bound({node, nullptr}); it != edges_.end() && it->first == node; ++it) {
            if (it->second == to) return true;
            if (seen.insert(it->second).second) stack.push_back(it->second);
        }
    }
    return false;
}

inline std::optional<std::string> LockRegistry::detect_and_break_cycle() {
    std::lock_guard lock(m_);
    if (!victims_.empty()) return std::nullopt;  // a victim is already backing out
    for (const auto& [start, ignored] : waiting_) {
        std::string description;
        std::unordered_set<std::thread::id> visited;
        std::thread::id current = start;
        // Follow: thread -> the mutex it waits for -> that mutex's owner -> ...
        while (visited.insert(current).second) {
            auto wait = waiting_.find(current);
            if (wait == waiting_.end()) break;
            auto owner = owner_.find(wait->second);
            if (owner == owner_.end()) break;
            description += std::format("{} waits for {} (held by {}); ", name_of(current), wait->second->name(),
                                       name_of(owner->second));
            current = owner->second;
            if (current == start) {
                victims_.insert(start);
                return description + std::format("-> cycle! victim: {}", name_of(start));
            }
        }
    }
    return std::nullopt;
}

// ----------------------------------------------------------------------------------- watchdog
class DeadlockWatchdog {
public:
    explicit DeadlockWatchdog(std::chrono::milliseconds period = 5ms)
        : thread_([this, period](std::stop_token stop) {
              registry().name_current_thread("watchdog");
              while (!stop.stop_requested()) {
                  if (auto cycle = registry().detect_and_break_cycle()) {
                      std::lock_guard lock(mutex_);
                      if (reports_.empty()) reports_.push_back(*cycle);
                      ++detected_;
                  }
                  std::this_thread::sleep_for(period);
              }
          }) {}

    [[nodiscard]] int detected() const {
        std::lock_guard lock(mutex_);
        return detected_;
    }
    [[nodiscard]] std::optional<std::string> first_report() const {
        std::lock_guard lock(mutex_);
        return reports_.empty() ? std::nullopt : std::optional(reports_.front());
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> reports_;
    int detected_ = 0;
    std::jthread thread_;  // last member: joined before the fields above are destroyed
};

}  // namespace roadmap::deadlock

// =====================================================================================================
// The bank: one buggy transfer and two fixes
// =====================================================================================================

namespace {

using namespace roadmap::deadlock;

struct Account {
    Account(int account_id, std::string name, long initial) : id(account_id), mutex(std::move(name)), balance(initial) {}
    int id;
    TrackedMutex mutex;
    long balance;
};

// BUG: locks `from` then `to`. Opposite transfers running together form an ABBA cycle.
void transfer_naive(Account& from, Account& to, long amount) {
    std::lock_guard first(from.mutex);
    std::this_thread::sleep_for(100us);  // real code does work here; it widens the deadlock window
    std::lock_guard second(to.mutex);
    from.balance -= amount;
    to.balance += amount;
}

// FIX 1: a global order. Always lock the account with the lower id first, whatever the transfer direction.
void transfer_ordered(Account& from, Account& to, long amount) {
    Account& low = from.id < to.id ? from : to;
    Account& high = from.id < to.id ? to : from;
    std::lock_guard first(low.mutex);
    std::this_thread::sleep_for(100us);
    std::lock_guard second(high.mutex);
    from.balance -= amount;
    to.balance += amount;
}

// FIX 2: std::scoped_lock acquires both with a deadlock-avoidance algorithm.
void transfer_scoped(Account& from, Account& to, long amount) {
    std::scoped_lock both(from.mutex, to.mutex);
    std::this_thread::sleep_for(100us);
    from.balance -= amount;
    to.balance += amount;
}

using TransferFn = void (*)(Account&, Account&, long);

void run_scenario(std::string_view title, TransferFn transfer, bool concurrent) {
    constexpr int kTransfers = 40;
    Account a(0, "account-A", 1'000), b(1, "account-B", 1'000);
    registry().reset_lock_order();
    std::atomic<int> retries{0};

    auto teller = [&](std::string name, Account& from, Account& to) {
        registry().name_current_thread(std::move(name));
        for (int i = 0; i < kTransfers; ++i) {
            for (;;) {  // like a database transaction: if chosen as deadlock victim, retry
                try {
                    transfer(from, to, 10);
                    break;
                } catch (const DeadlockError&) {
                    ++retries;
                }
            }
        }
    };

    std::println("{}", title);
    const auto start = std::chrono::steady_clock::now();
    DeadlockWatchdog watchdog;
    if (concurrent) {
        std::jthread t1(teller, "teller-1", std::ref(a), std::ref(b));
        std::jthread t2(teller, "teller-2", std::ref(b), std::ref(a));
    } else {  // strictly one after the other: a deadlock is impossible in this run
        std::jthread(teller, "teller-1", std::ref(a), std::ref(b)).join();
        std::jthread(teller, "teller-2", std::ref(b), std::ref(a)).join();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    std::println("   finished {} transfers in {} ms; total money {} (expected 2000)", 2 * kTransfers, ms.count(),
                 a.balance + b.balance);
    std::println("   wait-for graph: {} deadlock(s) detected and broken, {} retries", watchdog.detected(), retries.load());
    if (auto report = watchdog.first_report()) std::println("     first: {}", *report);
    const auto inversions = registry().inversions();
    std::println("   lock-order graph: {} potential deadlock(s)", inversions.size());
    for (const auto& line : inversions) std::println("     {}", line);
    std::println("");
}

// A real, uninstrumented deadlock with plain std::mutex, for practising with sample / lldb / gdb.
[[noreturn]] void hang_forever() {
    static std::mutex account_a, account_b;
    std::latch both_hold_first_lock(2);
    std::println("PID {} - about to deadlock for real. In another terminal run:", ::getpid());
    std::println("  macOS: sample {} 1        Linux: gdb -p {} -batch -ex 'thread apply all bt'", ::getpid(), ::getpid());
    std::println("Press Ctrl+C to quit.");
    std::fflush(stdout);  // the process will never exit normally, so flush now (matters when output is redirected)
    std::jthread t1([&] {
        std::lock_guard a(account_a);
        both_hold_first_lock.arrive_and_wait();  // guarantees the ABBA interleaving
        std::lock_guard b(account_b);
    });
    std::jthread t2([&] {
        std::lock_guard b(account_b);
        both_hold_first_lock.arrive_and_wait();
        std::lock_guard a(account_a);
    });
    for (;;) std::this_thread::sleep_for(1h);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "hang") hang_forever();

    run_scenario("1) BUG, concurrent: transfer_naive (lock from, then to), teller-1 A->B and teller-2 B->A",
                 transfer_naive, true);
    run_scenario("2) BUG, sequential: the same code with NO overlap - it cannot hang here, yet lockdep flags it",
                 transfer_naive, false);
    run_scenario("3) FIX 1, concurrent: transfer_ordered (lowest account id first)", transfer_ordered, true);
    run_scenario("4) FIX 2, concurrent: transfer_scoped (std::scoped_lock deadlock avoidance)", transfer_scoped, true);
}
