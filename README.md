# Engineering Interview Roadmap

A structured study plan and notes for software engineering interviews.

## Sections

| Folder | Topic | Status |
|---|---|---|
| [00-interview-strategy](./00-interview-strategy/) | Interview Strategy | Not started |
| [01-coding-fundamentals](./01-coding-fundamentals/) | Coding Fundamentals | Not started |
| [02-go-programming](./02-go-programming/) | Go Programming | Not started |
| [03-coding-practices](./03-coding-practices/) | Coding Practices | Not started |
| [04-system-design](./04-system-design/) | System Design | Not started |
| [05-system-programming](./05-system-programming/) | System Programming | Not started |
| [06-networking](./06-networking/) | Networking | Not started |
| [07-linux-internals](./07-linux-internals/) | Linux Internals | Not started |
| [08-kubernetes-cloud-native](./08-kubernetes-cloud-native/) | Kubernetes & Cloud Native | Not started |
| [09-ai-competencies](./09-ai-competencies/) | AI Competencies | Not started |
| [10-behavioural-interviews](./10-behavioural-interviews/) | Behavioural Interviews | Not started |
| [11-mock-interviews](./11-mock-interviews/) | Mock Interviews | Not started |
| [12-showcase-projects](./12-showcase-projects/) | Showcase Projects | Not started |
| [13-revision](./13-revision/) | Revision | Not started |

See [ROADMAP.md](./ROADMAP.md) for the plan and [PROGRESS.md](./PROGRESS.md) for tracking.

## C++ exercises

All in C++23, built by the root `CMakeLists.txt`.

| # | Exercise | Folder | Tests |
|---|---|---|---|
| 1 | Thread-safe bounded queue | [05-system-programming/concurrency-synchronization/bounded-queue](./05-system-programming/concurrency-synchronization/bounded-queue/) | yes + README |
| 2 | LRU cache, O(1) | [01-coding-fundamentals/data-structures/hash-maps/lru-cache](./01-coding-fundamentals/data-structures/hash-maps/lru-cache/) | yes + README |
| 3 | Concurrent TCP server | [06-networking/sockets/concurrent-tcp-server](./06-networking/sockets/concurrent-tcp-server/) | yes + README |
| 4 | Find and fix a data race | [05-system-programming/concurrency-synchronization/data-race-fix](./05-system-programming/concurrency-synchronization/data-race-fix/) | yes + README |
| 5 | Rate limiter (5 algorithms + per-key) | [04-system-design/scalability-performance/rate-limiter](./04-system-design/scalability-performance/rate-limiter/) | yes |
| 6 | Worker pool with graceful shutdown | [05-system-programming/processes-threads/worker-pool](./05-system-programming/processes-threads/worker-pool/) | demo only |
| 7 | Stream message framing | [06-networking/tcp-ip/stream-message-framing](./06-networking/tcp-ip/stream-message-framing/) | demo only |
| 8 | Producer-consumer with backpressure | [05-system-programming/concurrency-synchronization/producer-consumer-backpressure](./05-system-programming/concurrency-synchronization/producer-consumer-backpressure/) | demo only |
| 9 | Deadlock diagnosis | [05-system-programming/debugging-profiling/deadlock-diagnosis](./05-system-programming/debugging-profiling/deadlock-diagnosis/) | demo only |
| 10 | Concurrent cache with expiration | [04-system-design/databases-caching/expiring-cache](./04-system-design/databases-caching/expiring-cache/) | demo only |

The explanation of each design is in the exercise's `README.md` (1–4) or in the comment block at the top of its source file.

### Build and run

#### Requirements

| Tool | Version | Notes |
|---|---|---|
| CMake | 3.25 or newer | |
| C++ compiler with C++23 support | **GCC 14+** (tested) | Clang 19+ and MSVC 17.10+ should work but have not been tested |
| OS | macOS, Linux, or Windows via WSL2 | Exercises 3, 7 and 9 use POSIX APIs (sockets, `poll`, `getpid`), so a native Windows/MSVC build is not supported |

The library features used are `std::print`, `std::expected`, `std::move_only_function`, `std::jthread`, `std::stop_token`
and `std::latch`. A compiler or standard library that lacks any of them will fail to build.

#### Step 1: install the tools

```bash
# macOS (Homebrew)
brew install cmake gcc

# Ubuntu / Debian (Ubuntu 24.04 or newer ships g++-14)
sudo apt update && sudo apt install -y build-essential cmake g++-14

# Fedora (the default gcc is new enough)
sudo dnf install -y gcc-c++ cmake

# Arch Linux
sudo pacman -S --needed base-devel cmake
```

**Windows:** install WSL2 with Ubuntu from an administrator PowerShell (`wsl --install -d Ubuntu`), open the
Ubuntu terminal, and follow the Ubuntu / Debian instructions there.

#### Step 2: configure

Run from the repository root. The only platform-specific part is choosing the compiler.

```bash
# Linux (or WSL2): use g++-14 if your default g++ is older than 14
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14

# Linux where the default g++ is already 14 or newer (e.g. Fedora, Arch)
cmake -S . -B build

# macOS with Homebrew GCC
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14
```

**macOS:** if the configure step fails with *"You have not agreed to the Xcode license agreements"*, either accept
the license once (`sudo xcodebuild -license accept`) or point CMake at the Command Line Tools SDK instead:

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
```

Optional settings (add them to the configure command):

| Option | Effect |
|---|---|
| `-DCMAKE_BUILD_TYPE=Release` | Optimised build. The default is `RelWithDebInfo` (optimised, with debug symbols) |
| `-DCMAKE_BUILD_TYPE=Debug` | No optimisation, easiest to step through in a debugger |
| `-DSANITIZE=thread` | ThreadSanitizer: reports data races (Linux, or Clang on macOS) |
| `-DSANITIZE=address,undefined` | AddressSanitizer + UBSan: memory errors and undefined behaviour |
| `-G Ninja` | Use Ninja instead of Make (faster builds, if Ninja is installed) |

#### Step 3: build

```bash
cmake --build build --parallel
```

To build a single program, add `--target <name>` (names are in the table below).

#### Step 4: run the tests and programs

```bash
ctest --test-dir build --output-on-failure    # unit tests (exercises 1-5)
```

Every program is built in the `build/` folder at the same path as its source folder:

```bash
./build/05-system-programming/processes-threads/worker-pool/worker_pool
./build/06-networking/sockets/concurrent-tcp-server/concurrent_tcp_server_demo
```

| Exercise | Program | Unit tests |
|---|---|---|
| 1 Bounded queue | `bounded_queue_demo` | `bounded_queue_test` |
| 2 LRU cache | `lru_cache_demo` | `lru_cache_test` |
| 3 Concurrent TCP server | `concurrent_tcp_server_demo` (add `serve 9000` to run it interactively, then `nc 127.0.0.1 9000`) | `concurrent_tcp_server_test` |
| 4 Data race fix | `data_race_fix_demo` | `data_race_fix_test` |
| 5 Rate limiter | `rate_limiter_demo` | `rate_limiter_test` |
| 6 Worker pool | `worker_pool` | – |
| 7 Stream message framing | `stream_message_framing` | – |
| 8 Producer-consumer with backpressure | `producer_consumer_backpressure` | – |
| 9 Deadlock diagnosis | `deadlock_diagnosis` (add `hang` to create a real deadlock to inspect with a debugger) | – |
| 10 Expiring cache | `expiring_cache` | – |

#### Clean rebuild

```bash
rm -rf build      # then repeat steps 2 and 3
```
