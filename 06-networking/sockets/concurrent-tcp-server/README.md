# Concurrent TCP server

**What it tests:** sockets, concurrency, lifecycle management.

A TCP server that handles many clients at once (one thread per connection). It
limits how many connections it accepts, survives handler failures, and **shuts down
cleanly**: no thread is left running, no file descriptor is leaked, and no client is
left hanging.

| File | Purpose |
|---|---|
| [`socket.hpp`](socket.hpp) | RAII + `std::expected` wrappers over POSIX sockets |
| [`tcp_server.hpp`](tcp_server.hpp) | The server: accept loop, sessions, reaping, shutdown |
| [`test.cpp`](test.cpp) | 12 tests: concurrency, limits, failures, shutdown |
| [`main.cpp`](main.cpp) | Demo, plus an interactive mode you can try with `nc` |

## API

```cpp
using roadmap::net::TcpServer, roadmap::net::Connection;

auto server = TcpServer::start({.port = 8080, .max_connections = 100},
                               [](Connection& c) {          // runs on its own thread
                                   std::array<std::byte, 4096> buf;
                                   while (auto n = c.receive(buf); n && *n > 0)
                                       if (!c.send_all(std::span(buf).first(*n))) break;
                               });
if (!server) { std::println("{}", server.error().message()); return 1; }   // std::expected

(*server)->port();  (*server)->active_connections();  (*server)->total_rejected();
(*server)->stop();   // graceful; the destructor also calls it
```

## Architecture

```
                         ┌──────────────── accept thread ────────────────┐
  clients ──connect──►   │ poll([listening socket, wake pipe], 50 ms)    │
                         │   ├─ listener readable → accept() all pending │
                         │   │      └─ start_session(): new jthread ─────┼──► session thread: handler(conn)
                         │   ├─ wake pipe readable → re-check stop       │      session thread: handler(conn)
                         │   └─ every iteration  → reap_finished()       │      ...
                         └───────────────────────────────────────────────┘
  stop() ──writes 1 byte──► wake pipe
```

### Step 1: `start()`: bind before you spawn

`listen_tcp()` runs `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()`.
Only after all of these succeed do we create the server object and its accept thread.
`start()` returns `std::expected<std::unique_ptr<TcpServer>, std::error_code>`, so a
port that's already in use is a value the caller must handle, not a server half-started
in the background.

- **Port 0** asks the kernel for any free port; `getsockname()` tells us which one.
  The tests use this, so they never clash.
- **`SO_REUSEADDR`** lets you restart the server right away while old connections
  are still in `TIME_WAIT`.
- The server is held by `unique_ptr` because it can't move: its threads hold `this`.

### Step 2: the accept loop: `poll()` with a wake-up pipe

A thread blocked in `accept()` can't easily be woken from another thread. Closing the
listening socket under it is a race, and `shutdown()` on a listening socket behaves
differently on Linux and macOS. Instead, the accept thread waits in **`poll()`** on two
descriptors:

1. the **listening socket**: readable means a client is waiting to be accepted;
2. the read end of a **pipe** (the *self-pipe trick*): `stop()` writes one byte to
   it, which wakes `poll()` immediately on any OS.

The listening socket is **non-blocking**. Between `poll()` reporting it ready and our
`accept()` call, the client may have already reset the connection. A blocking
`accept()` would then hang. A non-blocking one returns `EAGAIN` and we go back to
`poll()`.

### Step 3: preparing each accepted socket

| Call | Why |
|---|---|
| `set_nonblocking(fd, false)` | **macOS/BSD: an accepted socket inherits `O_NONBLOCK` from the listener.** Our handlers use blocking I/O, so we clear it. Linux doesn't inherit it, but clearing is harmless. |
| `disable_sigpipe(fd)` | Writing to a closed peer raises `SIGPIPE`, which **kills the process** by default. macOS turns it off per socket (`SO_NOSIGPIPE`); Linux per call (`MSG_NOSIGNAL`). `socket.hpp` handles both. |
| `set_no_delay(fd)` | Disables Nagle's algorithm so small replies go out immediately instead of waiting for delayed ACKs. |
| `set_close_on_exec(fd)` | Stops the socket leaking into child processes. |

### Step 4: sessions, and how they are cleaned up

Each accepted client becomes a `Session { shared_ptr<const Socket>, std::jthread }`,
stored in `sessions_` (guarded by `mutex_`).

**A thread can't join itself.** So when a handler returns, its session thread just
adds its id to `finished_`. The accept thread wakes at least every `reap_interval`
(50 ms) and runs `reap_finished()`:

```
lock;  move finished sessions out of the map;  unlock;
destroy them  → jthread destructor joins the thread, the last shared_ptr closes the fd
```

The joins happen **outside the lock**: a session that is finishing needs the mutex to
report itself done, so joining while holding the mutex could deadlock.

**Why the thread is created while holding the mutex:** a very short handler could
finish and report itself before `sessions_[id].thread` was assigned. Because reporting
needs the mutex, the session can't report until the accept thread has finished
registering it.

**Why the socket is a `shared_ptr`:** `stop()` calls `shutdown()` on client sockets
while their handlers may be using them. If a handler could close its descriptor while
`stop()` was about to shut it down, the OS might have **reused that number for a
different file** in the meantime, and we'd shut down the wrong connection. Shared
ownership means the descriptor is closed only after *both* the handler and the server
are finished with it.

### Step 5: load shedding at `max_connections`

Thread-per-connection costs one thread (and its stack) per client. When the limit is
reached, new clients are accepted and then **immediately closed**, so they see EOF
and can retry elsewhere. Without a limit, a flood of idle connections would exhaust
threads or memory. (The kernel completes the TCP handshake before `accept()`, so the
client's `connect()` succeeds either way.)

### Step 6: handler failures are isolated

`run_session` wraps the handler in `try { … } catch (...)`. If an exception escapes a
`std::thread`, it calls `std::terminate` and the **whole process** dies. With the
catch, one bad request drops only its own connection; `handler_errors()` counts them.

### Step 7: `stop()`: graceful shutdown, in a deliberate order

```cpp
void stop() { std::call_once(stop_once_, [this] { shutdown(); }); }
```

1. **Stop accepting.** `request_stop()` + one byte to the wake pipe → the accept thread
   leaves `poll()` and exits. We join it, so **no new session can appear** after this.
2. **Wake every session.** For each live session: `request_stop()` (handlers can check
   `conn.stop_requested()`) and **`shutdown(fd, SHUT_RDWR)`**. Shutdown is what wakes a
   handler blocked in `recv()` on a client that will never send anything: `recv`
   returns 0, just as if the client had closed.
3. **Join every session thread**, outside the lock.
4. **Close the listening socket**, releasing the port. (A test checks the port can be
   reused immediately.)

`std::call_once` makes `stop()` idempotent and safe to call from several threads at
once: concurrent callers *wait* until shutdown is finished, rather than returning
early while threads are still running. The destructor calls `stop()`, so a server can
never be destroyed with threads still using it.

The order matters. If sessions were woken *before* the accept thread stopped, a
client connecting at that moment could start a new session after we'd finished
waking them, and nothing would ever wake it.

## A real bug these tests caught

The first version failed `stop_wakes_handlers_blocked_on_silent_clients`:
`active_connections()` returned **18446744073709551615**, which is `0 - 1` on an
unsigned value.

During `stop()`, `shutdown()` *takes over* the whole `sessions_` map (it swaps it into
a local) to join the threads. The woken handlers then still added their ids to
`finished_`, so `finished_` held ids that were no longer in `sessions_`, and
`sessions_.size() - finished_.size()` went below zero. The fix states an
**invariant** — *every id in `finished_` is also a key of `sessions_`* — and enforces
it with `if (sessions_.contains(id)) finished_.push_back(id);`.

This bug could only appear while shutting down with handlers still active. That's
why shutdown paths need their own tests.

## Error handling style

`socket.hpp` turns every POSIX call into `std::expected<T, std::error_code>`:

```cpp
auto n = socket.receive(buffer);          // std::expected<std::size_t, std::error_code>
if (!n) log(n.error().message());         // e.g. "Connection reset by peer"
else if (*n == 0) /* peer closed (EOF) */;
```

- `errno` is captured **immediately** (`last_error()`), before another call can
  overwrite it.
- `EINTR` (a call interrupted by a signal) is retried automatically.
- `send_all` loops: `send()` may accept only part of the buffer (a *short write*).
- Error codes compare to portable conditions: `error == std::errc::address_in_use`.

## TCP is a byte stream

`send("hello")` followed by `recv()` is **not** guaranteed to return `"hello"`. It
may return `"hel"`, then `"lo"`. Message boundaries don't exist in TCP. The tests use
a `read_exact(n)` loop for this reason. Splitting a byte stream back into messages
(framing) is exercise 7: [stream-message-framing](../../tcp-ip/stream-message-framing/).

## Concurrency model: thread-per-connection and alternatives

| Model | How | Good for | Cost |
|---|---|---|---|
| **Thread per connection** (this) | blocking I/O, one thread each | simple code, up to a few thousand clients | ~1 thread + stack per client |
| Thread pool + blocking I/O | queue accepted sockets to N workers | bounded threads | slow clients occupy workers |
| **Event loop** (`epoll` / `kqueue`) | one thread, non-blocking sockets, readiness events | 10k+ connections (C10K) | callback/state-machine code |
| Event loop × N cores | `SO_REUSEPORT`, one loop per core | nginx/Envoy-scale | more complex |
| io_uring / coroutines | completion-based async I/O | highest throughput on Linux | Linux-only, newest APIs |

## Tests

| Test | What it proves |
|---|---|
| `serves_clients_concurrently` | 8 handlers are running **at the same time** (impossible for a serial server) |
| `many_clients_many_round_trips` | 16 clients × 200 echoes, every byte correct |
| `stop_wakes_handlers_blocked_on_silent_clients` | `stop()` returns in < 1 s even though a client never sends anything |
| `rejects_clients_beyond_max_connections` | Load shedding; admitted clients unaffected |
| `finished_sessions_are_reaped` | 20 sequential clients → 0 active sessions afterwards (no thread leak) |
| `a_throwing_handler_only_drops_its_own_connection` | Exception isolation |
| `stop_is_idempotent_and_thread_safe` | Concurrent `stop()` calls + destructor |
| `port_is_released_after_stop` / `start_fails_cleanly_when_port_is_taken` | Port lifecycle, `EADDRINUSE` as a value |

## Build and run

```bash
cmake --build build --target concurrent_tcp_server_test concurrent_tcp_server_demo
./build/06-networking/sockets/concurrent-tcp-server/concurrent_tcp_server_test
./build/06-networking/sockets/concurrent-tcp-server/concurrent_tcp_server_demo          # self-contained
./build/06-networking/sockets/concurrent-tcp-server/concurrent_tcp_server_demo serve 9000
nc 127.0.0.1 9000        # in another terminal: type text, get it back in UPPER CASE; Ctrl+C the server
```

## Interview follow-ups

- **What happens to a client when `max_connections` is reached?** It's accepted
  and then closed. The alternative is to stop calling `accept()`, so new clients wait
  in the kernel backlog until it fills and connects start timing out.
- **Slow or malicious clients (slowloris)?** Add read/idle timeouts (`SO_RCVTIMEO`,
  or a deadline in an event loop) and limit the size of each request.
- **`SO_REUSEADDR` vs `SO_REUSEPORT`?** The first allows rebinding over `TIME_WAIT`.
  The second lets several sockets listen on the *same* port, so the kernel can spread
  connections across processes or threads.
- **How would you make it async?** Replace session threads with `kqueue`/`epoll`,
  keep a per-connection state machine and buffers, and handle `EAGAIN` instead of
  blocking.
- **TLS?** Wrap the socket (OpenSSL/BoringSSL). Framing and shutdown then also need
  the TLS `close_notify` message.
