# Thread-safe bounded queue

**What it tests:** synchronization, condition variables, shutdown.

A fixed-capacity FIFO queue that many producer and consumer threads can use at the
same time. Producers wait when it is full, consumers wait when it is empty, and
`close()` shuts it down gracefully.

| File | Purpose |
|---|---|
| [`bounded_queue.hpp`](bounded_queue.hpp) | The implementation (header-only, C++23) |
| [`test.cpp`](test.cpp) | 13 unit and stress tests |
| [`main.cpp`](main.cpp) | Demo: fast producers throttled by slow consumers, then a graceful close |

## API

```cpp
roadmap::BoundedQueue<Job> q(64);          // capacity must be > 0

bool ok = q.push(job);                     // blocks while full;  false if closed
bool ok = q.push(job, stop_token);         // ...or until stop is requested
bool ok = q.push_for(job, 100ms);          // ...or until the timeout expires
bool ok = q.try_push(job);                 // never blocks

std::optional<Job> j = q.pop();            // blocks while empty; nullopt = closed AND drained
std::optional<Job> j = q.pop(stop_token);  // ...or until stop is requested
std::optional<Job> j = q.pop_for(100ms);   // ...or until the timeout expires
std::optional<Job> j = q.try_pop();        // never blocks

q.close();                                 // graceful shutdown (idempotent)
```

The typical consumer loop is a single line, and it ends by itself at shutdown:

```cpp
while (auto job = q.pop()) process(*job);
```

## How it works

### State and invariants

```cpp
std::mutex                  mutex_;      // guards everything below
std::deque<T>               items_;      // the FIFO
bool                        closed_;     // set once by close(), never cleared
std::condition_variable_any not_full_;   // producers wait here
std::condition_variable_any not_empty_;  // consumers wait here
```

Every access to `items_` and `closed_` happens while `mutex_` is held, which gives
two invariants:

1. `items_.size() <= capacity_` at all times.
2. Once `closed_` is true, no new item is ever added.

### Waiting on a condition variable

A condition variable doesn't remember notifications; it only wakes threads that are
already waiting. So the waiter must always check the actual state (the **predicate**)
while holding the mutex:

```cpp
std::unique_lock lock(mutex_);
not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
```

`wait(lock, pred)` is shorthand for:

```cpp
while (!pred()) {
    not_full_.wait(lock);   // atomically: release the mutex + sleep; re-acquire on wake-up
}
```

The loop covers the three classic pitfalls:

- **Lost wake-up:** if the item arrived *before* we started waiting, `pred()` is
  already true and we never sleep. Checking state and going to sleep happen under the
  same mutex, so a notification can't slip in between them.
- **Spurious wake-up:** a thread may wake without being notified. The loop simply
  checks again and goes back to sleep.
- **Stolen wake-up:** with many consumers, another thread may take the item before we
  get the mutex back. The loop catches that too.

### Why two condition variables

Producers and consumers wait for different things: space versus items. With a
single condition variable, `notify_one()` after a push could wake *another producer*,
which finds the queue still full and goes back to sleep, while the consumer that
should have woken keeps sleeping. Separate `not_full_` and `not_empty_` mean each
notification reaches a thread that can make progress, so `notify_one()` is enough.

### Push and pop step by step

```
push(value)                                 pop()
-----------                                 -----
lock mutex                                  lock mutex
wait until closed || size < capacity        wait until closed || !empty
if closed: return false  (value untouched)  if empty: return nullopt  (closed and drained)
items.emplace_back(value)                   value = move(items.front()); items.pop_front()
unlock mutex                                unlock mutex
not_empty.notify_one()                      not_full.notify_one()
return true                                 return value
```

**Notifying after unlocking.** If we notified while still holding the mutex, the
woken thread would wake up only to block straight away on the mutex we still hold.
Unlocking first lets it take the mutex immediately. Both orders are correct; this one
just avoids a pointless context switch.

### Graceful shutdown with `close()`

```cpp
void close() {
    { std::lock_guard lock(mutex_); closed_ = true; }
    not_full_.notify_all();
    not_empty_.notify_all();
}
```

- `closed_` is part of **both** predicates, so every blocked thread (producer or
  consumer) wakes, re-checks, and returns.
- `notify_all` is required here: *every* waiter must react, not just one.
- **Producers** get `false`; their value is never consumed.
- **Consumers** keep receiving items that were queued before `close()`. Only when the
  queue is empty *and* closed does `pop()` return `std::nullopt`. Nothing that was
  accepted is ever lost.

Without `close()`, the only way to stop consumers would be a "poison pill" item (one
per consumer) or a killed thread. A closable queue makes shutdown part of the API.

### Cancelling a single wait: `std::stop_token`

Sometimes you want to stop *one* thread without closing the queue for everyone. C++20's
`std::condition_variable_any` can wait on a `std::stop_token`:

```cpp
not_empty_.wait(lock, stop, [this] { return closed_ || !items_.empty(); });
```

It returns early when `stop.request_stop()` is called. This pairs naturally with
`std::jthread`, which passes its own token into the thread function:

```cpp
std::jthread worker([&](std::stop_token st) { while (auto j = q.pop(st)) process(*j); });
worker.request_stop();   // wakes this worker only; the queue stays open
```

`condition_variable_any` is slightly heavier than `condition_variable` (it carries an
internal mutex). It's the only standard way to make a wait interruptible by a stop
token, and that's worth the cost here.

### A failed push doesn't consume your value

```cpp
template <typename U = T> requires std::constructible_from<T, U&&>
bool try_push(U&& value);
```

Push methods take a **forwarding reference** and forward the value into the deque
*only on success*. If `try_push(std::move(ptr))` fails because the queue is full,
`ptr` still owns its object, so the caller can retry or handle it. Taking `T` by value
would move the object into the parameter and destroy it on failure.

### Exception safety

- `items_.emplace_back(...)` gives the strong guarantee: if the copy or move throws,
  the queue is unchanged.
- `pop()` moves the front element into an `optional` **before** `pop_front()`, so if
  that move throws, the item stays in the queue.

## Complexity

| Operation | Time | Notes |
|---|---|---|
| push / pop | O(1) amortized | plus waiting time when full or empty |
| close | O(waiters) | `notify_all` |
| Memory | O(capacity) | bounded by design |

## Tests

| Test | What it proves |
|---|---|
| `preserves_fifo_order` | FIFO semantics |
| `try_push_fails_when_full_without_consuming_value` | Failed push leaves a `unique_ptr` intact |
| `push_blocks_while_full_until_a_pop` / `pop_blocks_until_an_item_arrives` | Blocking behavior |
| `close_wakes_blocked_consumers` / `..._producers` | `close()` wakes every waiter |
| `close_drains_items_already_queued` | No accepted item is lost during shutdown |
| `stop_token_cancels_a_blocked_pop` / `..._push` | Per-thread cancellation |
| `timed_operations_time_out` | `push_for` / `pop_for` |
| `mpmc_stress_every_item_delivered_exactly_once` | 4 producers × 25,000 items, 4 consumers: no item lost, none duplicated, capacity never exceeded |

## Build and run

From the repository root (see the root README for compiler setup):

```bash
cmake --build build --target bounded_queue_test bounded_queue_demo
./build/05-system-programming/concurrency-synchronization/bounded-queue/bounded_queue_test
./build/05-system-programming/concurrency-synchronization/bounded-queue/bounded_queue_demo
```

## Interview follow-ups

- **Why `while` and not `if` around `wait`?** Spurious wake-ups, and another thread
  may take the item first.
- **Lock-free alternative?** A ring buffer with atomic head and tail indices.
  Single-producer/single-consumer is straightforward; multi-producer/multi-consumer
  needs per-slot sequence numbers (Vyukov's queue). It's faster but much harder to
  get right, and it can't block a thread by itself.
- **Fairness:** `std::mutex` doesn't guarantee FIFO wake-up order. A strictly fair
  queue needs a ticket per waiter.
- **Batching:** `pop_all()` / `push_range()` reduce lock round-trips under high load.
- **Unbounded queue?** It turns a slow consumer into an out-of-memory crash. The
  bound is what gives you backpressure (see `producer-consumer-backpressure`).
