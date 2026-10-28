# Design patterns

A design pattern is a named, reusable solution to a problem that keeps coming up in software design.
It is not a library you install. It's a way of arranging classes and responsibilities that experienced
engineers recognize. Knowing the names lets you say "put a Strategy here" instead of describing the
whole structure.

Every pattern below has a runnable C++23 program in its own folder. The comment block at the top of
each `main.cpp` walks through that implementation.

| Pattern | Category | Program | Solves |
|---|---|---|---|
| [Strategy](#strategy) | Behavioral | [`strategy/`](strategy/main.cpp) | Swap one algorithm for another without changing the caller |
| [State](#state) | Behavioral | [`state/`](state/main.cpp) | Behavior that changes with an object's current state |
| [Observer](#observer) | Behavioral | [`observer/`](observer/main.cpp) | Notify many interested parties when something changes |
| [Adapter](#adapter) | Structural | [`adapter/`](adapter/main.cpp) | Use a class whose interface doesn't match the one you need |
| [Factory](#factory) | Creational | [`factory/`](factory/main.cpp) | Create objects without naming their concrete class |
| [Event-driven architecture](#event-driven-architecture) | Architectural | [`event-driven/`](event-driven/main.cpp) | Components that react to events instead of calling each other |
| [RAII](#raii) | C++ idiom | [`raii/`](raii/main.cpp) | Never forget to release a resource |
| [Producer–Consumer](#producer-consumer) | Concurrency | [`producer-consumer/`](producer-consumer/main.cpp) | Hand work between threads running at different speeds |
| [Thread Pool](#thread-pool) | Concurrency | [`thread-pool/`](thread-pool/main.cpp) | Run many small tasks on a fixed set of reusable threads |
| [Memory & resource management](#memory-and-resource-management-design) | Design discipline | [`resource-management/`](resource-management/main.cpp) | Clear ownership, no leaks, fast allocation |

**Build and run** (from the repository root, after configuring as in the root [README](../../README.md#build-and-run)):

```bash
cmake --build build --target strategy_pattern
./build/03-coding-practices/design-patterns/strategy/strategy_pattern
```

Program names: `strategy_pattern`, `state_pattern`, `observer_pattern`, `adapter_pattern`, `factory_pattern`,
`event_driven_architecture`, `raii_pattern`, `producer_consumer_pattern`, `thread_pool_pattern`, `resource_management`.

---

## The principles behind the patterns

Most patterns apply a small number of general principles:

- **Encapsulate what varies.** Find the part of the code that keeps changing and give it its own
  interface, so the rest of the code stays the same.
- **Program to an interface, not an implementation.** Code that depends on `ShippingStrategy` doesn't
  care which shipping class it gets.
- **Prefer composition over inheritance.** Build behavior by holding objects rather than deriving from
  them; you can swap them at run time and avoid deep hierarchies.
- **Open/closed principle.** Code should be open for extension (add a new type) but closed for
  modification (don't edit existing, tested code to do it).
- **Single responsibility.** Each class has one reason to change.
- **Loose coupling.** The less two components know about each other, the more freely each can change.

In C++ there's a further theme: **ownership and lifetime are part of the design.** RAII and resource
management aren't optional extras; they decide whether the code is correct.

---

## Strategy

**Category:** behavioral · **Program:** [`strategy/main.cpp`](strategy/main.cpp)

**Intent.** Define a family of algorithms, put each one behind a common interface, and make them
interchangeable. The code that uses the algorithm (the *context*) doesn't change when a new variant is added.

**Problem it solves.** Without the pattern, alternatives turn into conditionals spread through the code:

```cpp
double shipping(const Order& o) {
    if (mode == "flat") ...
    else if (mode == "weight") ...
    else if (mode == "free-over-100") ...   // every new rule edits this function, and its tests
}
```

**Structure.**

```
 Context (Checkout) ──holds──► «interface» Strategy ◄─── FlatRate
       calls strategy->cost()       cost(order)     ◄─── ByWeight
                                                    ◄─── FreeOverThreshold
```

- *Strategy*: the common interface (`ShippingStrategy::cost`)
- *Concrete strategies*: the interchangeable algorithms
- *Context*: holds a strategy and delegates to it. It may let callers swap the strategy at run time.

**Three ways to write it in C++** (all three are in the program):

| Form | How | When |
|---|---|---|
| Virtual interface | `std::unique_ptr<Strategy>` | Strategy chosen at run time, strategies have state, a stable API |
| `std::function` | Any lambda or function | Lightweight; one-off strategies; composing strategies |
| Template policy | `template <ShippingPolicy P>` + a C++20 `concept` | Strategy fixed at compile time; zero overhead, can be inlined |

**Strategy vs State.** Both delegate to an object behind an interface. In *Strategy* the **client**
chooses the algorithm and the strategies don't know about each other. In *State* the **states
themselves** decide when to switch to another state.

**Use it when** you have several ways to do one job (sorting, pricing, compression, retry back-off,
routing, authentication) and the choice depends on configuration, input or the customer.

**Pitfalls.** Don't use it when there will only ever be one or two fixed variants; an `if` is simpler.
Clients must know enough to choose a strategy.

**Real-world examples.** The comparator passed to `std::sort`; allocator and hash template parameters in
the standard library; payment methods; load-balancing algorithms (round-robin, least-connections).

---

## State

**Category:** behavioral · **Program:** [`state/main.cpp`](state/main.cpp)

**Intent.** Let an object change its behavior when its internal state changes, as if it had changed
class. Each state is its own type, which knows how to handle each event and which state comes next.

**Problem it solves.** Objects with a lifecycle (orders, documents, connections, protocols) often end up
with the same `switch (state_)` copied into every method. Adding a state means editing all of them, and
it's easy to allow an invalid transition by mistake.

**Structure (classic form).**

```
 Context (Document) ──holds──► «interface» State ◄── Draft
   edit() → state_->edit(*this)    edit / submit /  ◄── InReview
   transition_to(next)             approve / reject ◄── Published
```

The context forwards every request to its current state object. A state handles the request and may
call `context.transition_to(new_state)`. A base class that refuses every action by default means each
state only overrides what it actually allows.

**Modern C++ form: `std::variant` state machine.** The states are plain structs held in a
`std::variant`, and the transition table is a set of overloaded functions chosen by `std::visit` on
(state, event):

```cpp
using State = std::variant<Disconnected, Connecting, Connected>;
State operator()(Connecting c, Failed) const { return c.attempt < 3 ? Connecting{c.attempt + 1} : Disconnected{}; }
```

Advantages over the classic form:
- Each state holds only the data that's valid in it (`Connected` has a session id, `Disconnected` doesn't).
- There are no heap allocations.
- Every (state, event) pair is either a listed transition or the explicit fallback, so nothing slips through.

**State machine vocabulary.** *States*, *events* (inputs), *transitions* (state + event → new state),
*guards* (conditions on a transition, e.g. "retry only if attempts < 3"), and *actions* (side effects
on entry, exit or transition).

**Use it when** behavior depends on a lifecycle with clear states: protocol handlers (TCP's own state
machine), parsers, UI flows, order processing, game characters, device drivers.

**Pitfalls.** For two or three states a simple `enum` + `switch` is clearer. Transitions spread across
many state classes can be hard to see as a whole; document the state diagram. In the classic form,
watch for a state object being destroyed while one of its own methods is still running (the
`transition_to` call replaces it).

---

## Observer

**Category:** behavioral · **Program:** [`observer/main.cpp`](observer/main.cpp)

**Intent.** Define a one-to-many dependency: when one object (the *subject*) changes, all its
*observers* are notified automatically. The subject doesn't know what the observers are.

**Structure.**

```
 Subject (StockTicker)                       Observers
   subscribe(callback) ──► list ──notify──►  logger, averager, PriceAlert, ...
   emit(event)
```

Also called *publish/subscribe* (in-process), *signals and slots* (Qt, Boost.Signals2), *listeners*
(Java), or *event handlers*.

**The naive version has real bugs**, which the program addresses:

| Problem | What goes wrong | Fix in the program |
|---|---|---|
| Dangling observer | Observer is destroyed but still in the list → call through a dead pointer | `subscribe()` returns a move-only `Subscription`; its destructor unsubscribes (RAII) |
| Changing the list during notification | Observer unsubscribes while `emit()` is iterating → iterator invalidated | `emit()` iterates over a **snapshot** of the list |
| Threads | One thread subscribes while another emits → data race | List guarded by a mutex; callbacks run **outside** the lock to avoid deadlocks |
| Subject dies first | `Subscription` destructor touches a dead subject | `Subscription` holds a `weak_ptr` to the subject's state |
| Observer lifetime tied to an object | Forgetting to unsubscribe | `observe_weak(shared_ptr, &T::method)` stops calling once the object is gone |

**Push vs pull.** *Push*: the event carries the data (`PriceTick{symbol, price}`). *Pull*: the
observer is told "something changed" and queries the subject. Push is simpler and avoids races;
pull avoids sending data nobody needs.

**Use it when** several independent parts must react to changes in one place: UI updates (model →
views, as in MVC), configuration reloads, domain events, caches that must be invalidated, metrics.

**Pitfalls.**
- Notification order is usually unspecified; don't depend on it.
- A slow observer slows every `emit()`. Use an [event-driven](#event-driven-architecture) queue to
  decouple them in time.
- Observers that trigger other notifications can cause cascades or infinite loops.
- Many hidden observers make the flow hard to follow and debug.

---

## Adapter

**Category:** structural · **Program:** [`adapter/main.cpp`](adapter/main.cpp)

**Intent.** Convert the interface of an existing class into the interface the client expects, so
classes that couldn't work together can. Also called a *wrapper*.

**Structure.**

```
 Client ──uses──► «interface» Target              Adaptee (can't change)
                  read_celsius()                  GetTempTenthsF()  // -1 on error
                        ▲
                        │ implements
                     Adapter ──holds──► Adaptee
                     read_celsius() { convert units, convert errors, call adaptee }
```

**Two forms:**

| | Object adapter (composition) | Class adapter (inheritance) |
|---|---|---|
| How | Adapter **holds** an adaptee | Adapter **inherits** from the adaptee (privately) |
| Works with | The adaptee and any subclass of it; you can choose the instance at run time | One specific adaptee class |
| Can override adaptee behavior | No | Yes |
| Usually preferred | **Yes** (composition over inheritance) | When you need access to protected members |

**What an adapter translates**, all shown in the program:
- **Names and signatures:** `GetTempTenthsF()` → `read_celsius()`
- **Units and data formats:** tenths of °F, Kelvin, the C string `"23.3C"` → °C as a `double`
- **Error conventions:** `-1`, `errno`-style `int` codes → `std::expected<double, std::string>`
- **Resource conventions:** C `open`/`close` → RAII (`unique_ptr` with a custom deleter)
- **Calling conventions:** a C callback (function pointer + `void*` user data) → any C++ lambda

**Use it when** integrating legacy code, third-party libraries, C APIs or a vendor SDK; when two
teams' interfaces don't match; or to keep a library's types out of your own code (an
*anti-corruption layer*).

**Related patterns.**
- **Facade:** gives a *simpler* interface to a whole subsystem. Adapter makes *one* interface match
  *another*.
- **Decorator:** keeps the *same* interface and adds behavior.
- **Proxy:** keeps the same interface and controls access to the object.

---

## Factory

**Category:** creational · **Program:** [`factory/main.cpp`](factory/main.cpp)

**Intent.** Separate *creating* objects from *using* them. Callers ask for "an exporter" and get an
object behind an interface, without naming, constructing or even `#include`-ing the concrete class.

**Why it matters.** `new CsvExporter` hard-codes a dependency on one class. Centralizing creation
means that choosing, configuring and replacing the concrete type happens in one place, and the rest of
the code depends only on interfaces.

**Four variants** (all in the program):

| Variant | Shape | Use it when |
|---|---|---|
| **Simple factory** | One function: `make_exporter(Format) → unique_ptr<Exporter>` (a switch) | A small, fixed set of types chosen by a value |
| **Factory method** (GoF) | A base class runs an algorithm and calls a virtual `create_transport()`; **subclasses** decide what to create | A framework knows *when* to create but subclasses know *what* |
| **Abstract factory** (GoF) | One factory interface creates a **family** of related objects (`create_button`, `create_checkbox`) | Objects must match each other (light theme vs dark theme, SQL dialect, cloud provider) |
| **Registry factory** | A `name → creator` map; each type **registers itself** | Plugins, or a type chosen by a config file or CLI; adding a type must not touch existing code |

**Factory method vs abstract factory.** A factory method creates **one** product, and the choice is made
by subclassing. An abstract factory creates a **family** of products, and the choice is made by which
factory object you pass in.

**C++ notes.**
- Return `std::unique_ptr<Base>`: it makes clear that ownership passes to the caller, and the caller
  can turn it into a `shared_ptr` if needed.
- Report "unknown type" as a value (`std::expected`, `std::optional`) rather than a null pointer.
- A function-local `static` registry is created on first use, safely even across threads. That avoids
  the *static initialization order fiasco*, where one global is used before another file has
  constructed it.
- A **self-registering** registry has a caveat: if a registration lives in a static library and nothing
  references that object file, the linker may drop it, and the type silently disappears.

**Pitfalls.** For one concrete type, a factory is just extra code. Registries make it harder to see which
types exist; log them, as the program does.

---

## Event-driven architecture

**Category:** architectural · **Program:** [`event-driven/main.cpp`](event-driven/main.cpp)

**Intent.** Build a system from components that **publish events** ("OrderPlaced") and **react to
events** they've subscribed to, instead of calling each other directly. The publisher doesn't know
who is listening, or whether anyone is.

**Structure.**

```
 OrderService ──post(OrderPlaced)──► ┌──────────────┐ ──► PaymentService ──post(PaymentCaptured)──┐
                                     │  Event bus   │ ──► InventoryService                        │
                                     │  (queue +    │ ──► Analytics (added later, nothing else    │
                                     │   dispatch)  │       changed)                              │
 ShippingService ◄──PaymentCaptured──│              │ ◄───────────────────────────────────────────┘
       └──post(OrderShipped)───────► └──────────────┘ ──► EmailService
```

**Core ideas.**
- **Event:** an immutable fact about something that *has happened*, named in the past tense
  (`PaymentCaptured`). Compare a *command*, which is a request to do something (`CapturePayment`).
- **Event bus / broker:** routes each event to its subscribers. In-process it's a dispatcher like the
  one in the program; across services it's Kafka, RabbitMQ, NATS, AWS SNS/SQS or EventBridge.
- **Synchronous vs asynchronous delivery.** `publish()` runs handlers immediately on the caller's thread.
  `post()` queues the event and an **event loop** delivers it later, so the publisher never waits for a
  slow handler. In the program, posting 3 orders took about 20 µs.
- **Choreography:** a workflow (pay → ship → email) emerges from services reacting to each other's
  events, with no central coordinator. The alternative is **orchestration**, where one component tells
  each step what to do. That's easier to follow, but more tightly coupled.
- **Failure isolation:** one failing handler must not stop the others. The program catches its
  exception, logs it, and keeps going.

**Benefits.**
- **Loose coupling:** add analytics or fraud checks without touching the order service.
- **Scaling:** consumers scale independently.
- **Resilience:** a slow consumer only builds a backlog; it doesn't make every call slow.
- **Audit trail:** the events are a record of what happened.

**Costs and how to handle them.**

| Challenge | Mitigation |
|---|---|
| Flow is hard to follow ("who reacts to this?") | Event catalog, tracing with a correlation id passed in every event |
| Eventual consistency: steps finish at different times | Design the UI and APIs for it; use the *saga* pattern for multi-step business transactions |
| Delivery is usually *at least once*, so duplicates happen | Make handlers **idempotent** (deduplicate by event id) |
| Ordering is only guaranteed per partition or key | Partition by entity id; don't rely on a global order |
| A failed event must not be lost | Retries with back-off, then a *dead-letter queue* |
| Publish + save to database must happen together | *Transactional outbox* pattern |
| Event shapes change over time | Versioned schemas, additive changes only |

**Related.**
- [Observer](#observer): event-driven architecture is Observer scaled up, with a queue so delivery is
  asynchronous.
- *Event sourcing:* store the events themselves as the source of truth.
- *CQRS:* separate write and read models, often fed by events.
- *Reactor* pattern: the event loop behind `epoll` / `kqueue` servers and Node.js.

---

## RAII

**Category:** C++ idiom · **Program:** [`raii/main.cpp`](raii/main.cpp)

**Intent.** *Resource Acquisition Is Initialization:* tie every resource to the lifetime of an object.
The **constructor acquires** the resource and the **destructor releases** it. C++ guarantees that
destructors run whenever an object leaves scope: normal return, early `return`, `break`, or an
exception unwinding the stack. So the release can't be forgotten or skipped.

**"Resource" means anything that must be released:** heap memory, files, sockets, mutex locks,
database connections and transactions, threads, GPU buffers, temporary files, a "busy" flag.

**Without RAII**, every exit path must remember to clean up, and an exception skips all of them:

```cpp
FILE* f = fopen(path, "w");
if (!write_header(f)) { fclose(f); return false; }   // easy to forget on one of many paths
process(f);                                         // throws -> f is never closed
fclose(f);
```

**With RAII:**

```cpp
File f(path, "w");      // throws if it can't open: no object means no resource
write_header(f);
process(f);             // throws -> ~File() closes it during stack unwinding
```

**Rules for writing an RAII class** (shown by `File` in the program):
1. Acquire in the constructor. If acquiring fails, **throw**, so an object never exists without its resource.
2. Release in the destructor, which must be **`noexcept`**. A destructor that throws during stack
   unwinding calls `std::terminate`.
3. Decide what copying and moving mean (the **rule of five**). A unique owner is **move-only**:
   delete copy, implement move, and make the moved-from object own nothing so it never releases twice.
4. Prefer the **rule of zero**: build classes from members that already manage themselves
   (`std::unique_ptr`, `std::vector`, `std::string`, `std::lock_guard`), and write no destructor at all.

**Destruction order.** Objects are destroyed in **reverse order of construction**, including during an
exception (the program shows `table lock → buffer → connection`). Later objects may depend on earlier
ones, so this order is always safe. It's also why the program declares the "delete partial file" guard
**before** the file: the file is closed first, then deleted.

**Variations.**
- **Scope guard** (`ScopeExit`, `ScopeFail`, `ScopeSuccess`): run any code when a scope ends, without
  writing a class. `ScopeFail` uses `std::uncaught_exceptions()` to act only when an exception is
  leaving the scope, for example to delete a half-written file.
- **Transaction / rollback guard:** undo changes unless `commit()` was called. This gives the *strong
  exception guarantee*: either the operation completes, or nothing changes.
- **Standard RAII types:** `std::unique_ptr` (with a custom deleter for C APIs), `std::shared_ptr`,
  `std::lock_guard` / `std::scoped_lock` / `std::unique_lock`, `std::jthread` (joins on destruction),
  `std::fstream`, all containers.

**Exception-safety guarantees**, which RAII makes achievable:

| Guarantee | Meaning |
|---|---|
| No-throw | The operation never throws (destructors, `swap`, moves should be here) |
| Strong | If it throws, the state is exactly as before (commit-or-rollback) |
| Basic | If it throws, nothing leaks and invariants hold, but the state may have changed |

**Other languages** have related tools: Python's `with`, Java's try-with-resources, C#'s `using`,
Go's `defer`. They need to be written at each call site, whereas a C++ destructor runs automatically.

---

## Producer-Consumer

**Category:** concurrency · **Program:** [`producer-consumer/main.cpp`](producer-consumer/main.cpp)

**Intent.** Decouple threads that **produce** work from threads that **consume** it, through a shared
**buffer**. Each side runs at its own pace, and neither needs to know about the other.

```
 producer ─┐                                 ┌─► consumer
 producer ─┼──put──► [ bounded buffer ] ──take──┤
 producer ─┘    (blocks when full)  (blocks when empty)  └─► consumer
```

**The buffer must solve three problems:**

| Problem | Requirement | Tool |
|---|---|---|
| Mutual exclusion | Two threads never modify the buffer at once | `std::mutex` |
| Empty buffer | Consumers **wait** (not spin, not fail) until an item arrives | semaphore `filled_slots`, or a condition variable |
| Full buffer | Producers **wait** until there's space, which bounds memory | semaphore `free_slots`, or a condition variable |

**Two classic implementations** (both in the program):
1. **Dijkstra's bounded buffer:** a ring buffer, two counting semaphores (`free_slots` starts at N,
   `filled_slots` at 0) and a mutex. `put` = `acquire(free)` → lock → write → unlock →
   `release(filled)`; `take` is the mirror image. C++20 has `std::counting_semaphore`.
2. **Channel / blocking queue:** a deque + mutex + two condition variables + `close()`. This is
   Go's buffered channel, Java's `BlockingQueue`, or Python's `queue.Queue`. With condition variables,
   always wait **with a predicate** (`cv.wait(lock, [&]{ return !items.empty(); })`): it handles
   spurious wake-ups and lost notifications.

**Stopping consumers cleanly.**
- **Poison pill:** send a special "stop" item, **one per consumer**, since each consumer must see its own.
- **Close:** mark the queue closed; consumers drain the remaining items and then receive "no more". This
  is cleaner, can't be miscounted, and works for any number of consumers.

**Why the buffer must be bounded.** An unbounded buffer turns a slow consumer into growing memory and
latency, and eventually an out-of-memory crash. The bound creates **backpressure**: producers are
slowed to the consumers' speed. For other overload policies (drop newest, drop oldest, time out) and
multi-stage pipelines, see
[`producer-consumer-backpressure`](../../05-system-programming/concurrency-synchronization/producer-consumer-backpressure/main.cpp).

**Sizing the buffer.** It only needs to absorb *bursts*. If producers are faster than consumers on
average, no buffer size helps; you need more consumers or less input. A larger buffer also means
higher latency for each item.

**Where it appears.** Logging (the app produces log lines, a background thread writes them), job and
task queues, network servers (an accept thread produces connections, worker threads consume them),
streaming pipelines, Unix pipes (`producer | consumer`), and message brokers between services.

---

## Thread Pool

**Category:** concurrency · **Program:** [`thread-pool/main.cpp`](thread-pool/main.cpp)

**Intent.** Create a fixed set of worker threads **once**, and reuse them to run many small tasks,
instead of creating and destroying a thread per task.

**Why.** Creating a thread costs tens of microseconds plus a stack (often 512 KB to 8 MB of reserved
memory). With a thread per request, a traffic spike can create thousands of threads that fight over the
CPU and exhaust memory. A pool **limits concurrency** and **amortizes thread creation**.

**Structure.** A thread pool is a [producer–consumer](#producer-consumer) system whose items are tasks:

```
 submit(task) ──► task queue(s) ──► worker 1 ─┐
 submit(task) ──►                 ──► worker 2 ─┼──► results via std::future
                                  ──► worker N ─┘
```

**Key design decisions:**

| Decision | Options |
|---|---|
| Number of threads | **CPU-bound** work: about the number of cores. **I/O-bound** (waiting) work: more. *Little's law*: threads ≈ throughput × time per task. The program shows 32 waiting tasks taking 231 ms with 4 threads and 35 ms with 32 |
| Queue | One shared queue (simple, but one lock all workers contend for) vs **one queue per worker + work stealing** |
| Results and errors | `submit()` returns a `std::future`; exceptions are stored in it and rethrown by `get()` |
| Queue limit | Unbounded (risk of running out of memory) vs bounded (`submit` blocks or rejects: backpressure) |
| Shutdown | **Drain:** finish queued tasks. **Cancel:** drop queued tasks, ask running ones to stop |

**Work stealing** (what the program implements; the design behind Java's `ForkJoinPool`, Intel TBB and
Go's scheduler):
- Each worker has its **own deque**, so workers rarely contend on a lock.
- A worker takes from the **back** of its own deque (LIFO: the newest task, whose data is still in the
  CPU cache).
- When idle, a worker **steals** from the **front** of another worker's deque (FIFO: the oldest task,
  usually the largest remaining piece of work).
- A task submitted *from* a worker goes to that worker's own deque. That's why recursive
  divide-and-conquer work spreads well: in the program a single root task fanned out across all 8
  workers through stealing.

**Splitting work.** Many small chunks balance better than one chunk per thread, because uneven chunks
leave threads idle at the end. The prime counter splits its range into 64 pieces for 8 threads and gets
about a 4× speed-up.

**Pitfalls.**
- **Deadlock by starvation:** a task that blocks waiting for another task queued in the *same* pool can
  hang the pool once every worker is waiting. Don't block inside a task on other tasks of the same
  pool; split the work differently, or use a pool that can run queued tasks while waiting.
- **Long blocking calls** (disk, network) occupy workers. Use a separate I/O pool, or async I/O.
- **Thread-local state** leaks between unrelated tasks that happen to run on the same thread.
- A task that throws must not kill its worker. `packaged_task` catches the exception and stores it in
  the future.

For a production-style pool with drain and cancel shutdown, stop tokens and a bounded queue, see
[`worker-pool`](../../05-system-programming/processes-threads/worker-pool/main.cpp).

---

## Memory and resource-management design

**Category:** design discipline · **Program:** [`resource-management/main.cpp`](resource-management/main.cpp)

**Intent.** For every resource, decide **who owns it**, **how long it lives**, and **where its memory
comes from**, and encode those decisions in types instead of comments. Most crashes and leaks in C++
(use-after-free, double free, leaks, dangling references) are ownership mistakes.

### 1. Ownership expressed in types

| Type | Meaning | Cost |
|---|---|---|
| `T` (value member) | Owned, lifetime tied to the owner | None |
| `std::unique_ptr<T>` | **Sole** owner; ownership can move, never copied | Same as a raw pointer |
| `std::shared_ptr<T>` | **Shared** ownership; freed when the last owner goes | Atomic reference count, extra control block |
| `std::weak_ptr<T>` | Non-owning **observer** of a `shared_ptr` object; `lock()` to use it if it's still alive | Same control block |
| `T&`, `T*`, `std::span`, `std::string_view` | **Borrowed:** no ownership, must not outlive the owner | None |

Guidelines:
- Default to values and `unique_ptr`.
- Use `shared_ptr` only when ownership is genuinely shared and lifetimes can't be predicted.
- In function signatures, take `unique_ptr<T>` by value to mean "I take ownership", and `const T&`
  to mean "I only use it".
- Never write `new` and `delete` by hand; use `make_unique` / `make_shared`.

### 2. Reference cycles

Reference counting can't free a cycle. If a `Team` holds `shared_ptr<Employee>` and the employee holds
`shared_ptr<Team>`, each keeps the other alive forever. The program's "leaky" team never runs its
destructor. **Fix:** decide which direction owns, and make the **back-pointer** a `weak_ptr` (or a raw
pointer when the parent is guaranteed to outlive the child). Typical places: parent ↔ child, observer
lists, caches, graphs.

### 3. Object pool

Reuse expensive objects (connections, threads, large buffers) instead of creating and destroying them.
The program's pool hands out a `unique_ptr` whose **custom deleter returns the object to the pool**, so
returning it is automatic (RAII again). The result: 2,000 leases, and 0 new connections were created.
Things to decide: maximum size, what happens when the pool is empty (grow, wait, or fail), and
**resetting an object's state** before it's reused.

### 4. Arena (region) allocation

General-purpose `new`/`delete` is flexible, but every allocation has a cost: locking or thread caches,
bookkeeping, fragmentation, and poor locality. When many objects **die together** (one request, one
frame, one parse tree, one compilation unit), allocate them from an **arena**: one large block,
handed out by moving a pointer forward, and **freed all at once**.

C++17 `std::pmr` (polymorphic memory resources) makes this easy to plug in without changing your types:

| Resource | Behavior |
|---|---|
| `std::pmr::monotonic_buffer_resource` | Arena: only moves forward, frees everything when destroyed. Can start from a stack buffer |
| `std::pmr::unsynchronized_pool_resource` / `synchronized_pool_resource` | Pools of fixed-size blocks; reuses freed memory |
| `std::pmr::new_delete_resource()` | The ordinary heap |
| `std::pmr::null_memory_resource()` | Always fails, a guard that proves nothing reached the heap |

The program builds a 200,000-node linked list both ways:
- **Ordinary heap:** 200,000 allocations.
- **Arena:** 19 heap allocations and about 5× faster.
- **A 4 KB static buffer** backing a `pmr::vector`, with the null resource upstream as a guard: 0 heap
  allocations.

Memory resources can also be **chained**: the program's `CountingResource` wraps another resource to
count what passes through. The same approach gives you limits, logging or leak detection.

### 5. Other techniques worth knowing

- **Reserve capacity** (`vector::reserve`) when the size is known, to avoid repeated reallocation.
- **Small-buffer optimization:** `std::string` and `std::function` store small values inline, with no
  heap allocation.
- **Avoid unnecessary copies:** move semantics, `std::span` / `string_view` for read-only views,
  passing large objects by `const&`.
- **Cache-friendly layout:** contiguous containers (`vector`) beat node-based ones (`list`, `map`) for
  iteration; group "hot" fields together; watch for **false sharing** between threads (`alignas(64)`).
- **Custom deleters** let `unique_ptr` manage anything: `FILE*` with `fclose`, sockets with `close`,
  `void*` from a C library with its free function.
- **Limits and back-pressure:** bounded queues and pools keep memory use bounded under load.

### 6. Finding problems

| Problem | Tool |
|---|---|
| Use-after-free, buffer overflow, double free | AddressSanitizer (`-DSANITIZE=address,undefined` in this project) |
| Leaks | LeakSanitizer (part of ASan on Linux), Valgrind, macOS `leaks` |
| Heap growth, allocation hot spots | heaptrack, Valgrind massif, Instruments (macOS) |
| Data races on shared objects | ThreadSanitizer (`-DSANITIZE=thread`) |

---

## Choosing a pattern: quick reference

| You need to… | Consider |
|---|---|
| Pick one of several algorithms at run time | Strategy |
| Change behavior as an object moves through a lifecycle | State |
| Tell several parts of the program that something changed | Observer |
| Decouple services so they can change and scale independently | Event-driven architecture |
| Use a class or C API whose interface doesn't fit | Adapter |
| Create objects without depending on their concrete class | Factory |
| Guarantee cleanup, even on errors | RAII |
| Pass work between threads that run at different speeds | Producer–Consumer |
| Run many short tasks without creating a thread for each | Thread Pool |
| Avoid leaks, dangling pointers and slow allocation | Memory and resource-management design |

## Resources

- *Design Patterns: Elements of Reusable Object-Oriented Software*: Gamma, Helm, Johnson, Vlissides (the "Gang of Four")
- *Head First Design Patterns*: Freeman & Robson (a friendlier introduction)
- *C++ Core Guidelines*, sections R (resource management) and CP (concurrency): https://isocpp.github.io/CppCoreGuidelines/
- *C++ Concurrency in Action*: Anthony Williams (thread pools, queues, synchronization)
- *Designing Data-Intensive Applications*: Martin Kleppmann (event-driven systems, messaging)
- https://refactoring.guru/design-patterns: illustrated catalog of the classic patterns
