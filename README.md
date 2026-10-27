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

Requires a C++23 compiler (GCC 14+ or a recent Clang) and CMake 3.25+.

```bash
# macOS with Homebrew GCC (works without accepting the Xcode license):
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
# Linux:  cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14

cmake --build build -j 8
ctest --test-dir build --output-on-failure        # unit tests for exercises 1-5

./build/05-system-programming/processes-threads/worker-pool/worker_pool   # any program, by folder
```

To build with sanitizers on Linux, add `-DSANITIZE=thread` or `-DSANITIZE=address,undefined`.
