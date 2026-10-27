# Find and fix a data race in shared state

**What it tests:** thread safety, atomics, mutexes.

Three realistic pieces of shared state, each with a different kind of concurrency bug,
plus the fix and the reasoning behind it:

| Case | Code | Bug | Fix |
|---|---|---|---|
| 1 | Request metrics | `++counter` from many threads → **lost updates** | `std::atomic` (lock-free) or a `std::mutex` |
| 2 | Bank transfers | Two balances updated separately → **broken invariant** | One mutex around the *whole* transfer |
| 3 | Config publication | "write data, then set a flag" → **stale or partial reads** | `release` / `acquire` + C++20 `atomic::wait` |

| File | Purpose |
|---|---|
| [`shared_state.hpp`](shared_state.hpp) | Buggy (`Unsafe*`, `AtomicFieldBank`) and fixed versions, with each bug commented in place |
| [`test.cpp`](test.cpp) | 9 tests showing the fixes are exact under heavy contention |
| [`main.cpp`](main.cpp) | Runs the buggy and fixed versions side by side so you can see the symptoms |

## What a data race is

A **data race** happens when two threads access the same memory location at the same
time, **at least one of them writes**, and **nothing orders the two accesses** (no
mutex, no atomic). In C++ a data race is **undefined behaviour**: the program has no
meaning at all, not just "a slightly wrong number".

Two different things are often called "race conditions":

- **Data race:** unsynchronized concurrent access to memory. Undefined behaviour. Fixed
  with atomics or locks.
- **Race condition (logic race):** every access is synchronized, but the *result*
  depends on timing because a multi-step operation isn't atomic *as a whole*. That's
  case 2. It's a design bug, and atomics on individual fields don't fix it.

## Case 1: lost updates in counters

```cpp
void record(std::uint64_t latency) {
    ++requests_;                   // BUG: load, add 1, store: three steps
    total_latency_ += latency;
    if (latency > max_latency_) max_latency_ = latency;   // BUG: check-then-act
}
```

### Why `++x` loses updates

`++requests_` isn't one operation. It's *load*, *add*, *store*:

```
thread A: load 41
                     thread B: load 41
thread A: store 42
                     thread B: store 42      ← A's increment is gone
```

### What really happened in the demo (the compiler is part of the race)

The demo ran 8 threads × 1,000,000 calls and counted **2,000,000 or 1,000,000** requests,
which are suspiciously round numbers. The generated ARM64 assembly
(`g++-14 -O2 -S`) shows why:

```asm
ldp   x8, x3, [x0]          ; load requests_ and total_latency_ ONCE, before the loop
L3:   ...                   ; the loop only updates registers
      bne   L3
add   x1, x8, 999424        ; requests_ + 1'000'000  (999424 + 576)
add   x1, x1, 576
stp   x1, x3, [x0]          ; store both ONCE, after the loop
```

Because a data race is undefined behaviour, the compiler may **assume no other thread
touches `requests_`**. It keeps the counter in a register for the whole loop and writes
it back once. Each thread's million increments became a single load and a single store,
so whichever thread stored last overwrote almost everyone else's work. This isn't a
compiler bug. The code had no defined meaning, so the compiler was free to do this.

### Fix A: `std::atomic` (lock-free)

```cpp
requests_.fetch_add(1, std::memory_order_relaxed);        // one indivisible read-modify-write
```

- `fetch_add` compiles to a single atomic instruction (`ldadd` on ARM64, `lock xadd`
  on x86). No increment can be lost, and the compiler can't keep the value in a register.
- **`memory_order_relaxed`** is enough: these counters don't protect any other data.
  We only need each one to be exact, not ordered with the rest of memory.
- **Atomic max** needs a **compare-and-swap loop** (there's no `fetch_max` before C++26):
  ```cpp
  auto current = max_.load(relaxed);
  while (latency > current && !max_.compare_exchange_weak(current, latency, relaxed)) {}
  ```
  It means: "if the max is still `current`, replace it; otherwise `current` is reloaded
  and we check again." `_weak` may fail spuriously, which is fine inside a loop.
- **False sharing:** the counters are `alignas(64)`, so each sits on its own cache line.
  Otherwise threads updating *different* counters would keep taking the same 64-byte
  cache line from each other. That's correct but slow.

### Fix B: one `std::mutex`

```cpp
void record(std::uint64_t latency) {
    std::lock_guard lock(mutex_);
    ++state_.requests;  state_.total_latency += latency;  state_.max_latency = std::max(...);
}
```

### Atomics vs mutex: which one?

| | `AtomicMetrics` | `LockedMetrics` |
|---|---|---|
| Each field exact | yes | yes |
| Snapshot is consistent (`total == sum`, matching `requests`) | **no**: fields are read at different moments | **yes** |
| Demo time, 8 threads × 1M calls (this Mac) | 669 ms | **191 ms** |

**Lock-free doesn't mean faster.** The atomic version does three contended atomic
operations per call, and each one bounces a cache line between cores. The mutex version
takes one lock and then updates plain memory. Pick based on correctness first (do you
need a consistent snapshot?), then measure. At very high throughput the real answer
is often **per-thread counters that are summed when read**, so there's no shared write
at all.

## Case 2: a broken invariant with no data race

Invariant: **the total amount of money in the bank never changes.**

```cpp
bool transfer(size_t from, size_t to, int64_t amount) {     // AtomicFieldBank
    if (balances_[from].load() < amount) return false;      // BUG 1: check-then-act
    balances_[from].fetch_sub(amount);
    //  <-- money has left `from` but hasn't arrived at `to`
    balances_[to].fetch_add(amount);                        // BUG 2: two separate atomic steps
    return true;
}
```

Every individual access is atomic, so there's **no data race** and no undefined
behaviour. It's still wrong:

- **Torn invariant:** an auditor summing balances between the two steps sees money
  missing. In the demo it saw a wrong total in **96% of 3 million audits**.
- **Overdraft (check-then-act):** two transfers can both pass the `>= amount` check
  before either subtracts, so the account goes negative.

**Fix:** the invariant covers several variables, so **one lock must cover all of them**,
for both writers and readers:

```cpp
bool transfer(size_t from, size_t to, int64_t amount) {     // LockedBank
    std::lock_guard lock(mutex_);
    if (from == to || balances_[from] < amount) return false;  // check...
    balances_[from] -= amount;                                 // ...and act, under ONE lock
    balances_[to]   += amount;
    return true;
}
std::int64_t total() const { std::lock_guard lock(mutex_); return std::reduce(...); }
```

**The rule:** *a lock protects an invariant, not a variable.* Ask "which facts must be
true together?" and make one lock cover all of them. With one lock per account you'd
need to lock both accounts in a transfer, which raises lock-ordering and deadlock
questions (exercise 9, deadlock diagnosis).

## Case 3: publishing data to another thread

The buggy pattern (shown in a comment in the header, not compiled):

```cpp
std::string config;  bool ready = false;
// writer:                    // reader:
config = load_config();       while (!ready) {}       // may spin forever
ready = true;                 use(config);            // may see a half-written string
```

Two separate problems:

1. **`ready` isn't atomic.** The compiler may read it once and loop on that value
   forever (the same register effect as in case 1).
2. **No ordering.** The CPU or compiler may reorder the writes, so a reader can see
   `ready == true` before the `config` writes are visible.

**Fix:** a *release* store paired with an *acquire* load:

```cpp
value_.emplace(std::move(value));                 // (1) ordinary write
ready_.store(true, std::memory_order_release);    // (2) publish
// reader:
ready_.wait(false, std::memory_order_acquire);    // C++20: sleep until ready_ != false
return *value_;                                   // guaranteed to see (1)
```

If an acquire load reads the value written by a release store, then **everything the
writer did before the release *happens-before* everything the reader does after the
acquire.** This is how mutexes work underneath: lock is an acquire, unlock is a release.

**C++20 `std::atomic::wait` / `notify_all`** let the reader *sleep* instead of spinning,
much like a futex. `std::atomic_flag::test_and_set` ensures `publish` runs only once.
For one-time initialization in production code, also consider `std::call_once` or a
function-local `static`, which C++ guarantees to initialize thread-safely.

## How to find data races in practice

1. **ThreadSanitizer**, the most effective tool: build with `-fsanitize=thread` and run
   the tests. It reports both stacks of every race. This project supports it:
   ```bash
   cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan && ctest --test-dir build-tsan
   ```
   (TSan isn't available with Homebrew GCC on macOS; use Linux, or Apple clang after
   accepting the Xcode license.)
2. **Stress tests:** many threads, a `std::latch` to start them all at once, invariant
   checks while running, repeated runs. The tests here use all three.
3. **Code review checklist:** for every shared variable, ask *who writes it, who reads
   it, and what synchronizes them?* Watch for `++` on shared fields, check-then-act
   (`if (x) use(x)`, `if (!map.contains(k)) map[k] = ...`), and invariants that span
   several fields.
4. **Clang's `-Wthread-safety`:** annotate members with `GUARDED_BY(mutex_)` and the
   compiler rejects unlocked accesses at build time.

## Tests

| Test | What it proves |
|---|---|
| `unsafe_metrics_is_correct_with_a_single_thread` | The bug is invisible without concurrency, which is why races reach production |
| `atomic_metrics_lose_no_updates` / `locked_..._no_updates` | 8 × 100,000 calls, counts exact |
| `atomic_max_survives_contention` | The CAS loop keeps the true maximum |
| `locked_metrics_snapshots_are_consistent` | A reader running alongside writers never sees `total != 10 × requests` |
| `locked_bank_never_creates_or_destroys_money` | 8 tellers × 50,000 random transfers plus a live auditor: 0 bad audits, no negative balances |
| `publisher_makes_the_whole_value_visible` | 6 blocked readers all see the complete value |

## Build and run

```bash
cmake --build build --target data_race_fix_test data_race_fix_demo
./build/05-system-programming/concurrency-synchronization/data-race-fix/data_race_fix_test
./build/05-system-programming/concurrency-synchronization/data-race-fix/data_race_fix_demo
```

The demo's "buggy" numbers change from run to run, between compilers and between
optimization levels. That's what undefined behaviour looks like.

## Interview follow-ups

- **Is `volatile` a fix?** No. `volatile` stops some compiler optimizations, but it
  gives neither atomicity nor inter-thread ordering. It's for memory-mapped hardware,
  not threads.
- **Is `x86` safe without atomics?** No. x86 hardware keeps stores ordered, but the
  *compiler* can still cache values in registers and reorder code, as the assembly above
  shows. `++x` isn't atomic on any architecture.
- **`shared_mutex` for readers?** It helps when reads greatly outnumber writes *and*
  the critical section is long. For short sections a plain mutex is often faster.
- **`seq_cst` vs `acq_rel` vs `relaxed`:** use `seq_cst` (the default) unless you can
  explain the happens-before relationships you rely on. Use `relaxed` only for
  standalone counters and flags that guard no other data.
