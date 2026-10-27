// Demo: 2 fast producers, 3 slow consumers, a queue of capacity 4.
// Watch producers get throttled when the queue is full, then a graceful close()
// that lets consumers drain every remaining item before they exit.

#include "bounded_queue.hpp"

#include <chrono>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
std::mutex print_mutex;

template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::lock_guard lock(print_mutex);
    std::println(fmt, std::forward<Args>(args)...);
}
}  // namespace

int main() {
    roadmap::BoundedQueue<std::string> queue(4);

    std::vector<std::jthread> consumers;
    for (int id = 0; id < 3; ++id) {
        consumers.emplace_back([&queue, id] {
            while (auto job = queue.pop()) {
                log("  consumer {} <- {}   (queue size {})", id, *job, queue.size());
                std::this_thread::sleep_for(30ms);  // consumers are slower than producers
            }
            log("  consumer {} exits: queue closed and drained", id);
        });
    }

    {
        std::vector<std::jthread> producers;
        for (int id = 0; id < 2; ++id) {
            producers.emplace_back([&queue, id] {
                for (int n = 0; n < 6; ++n) {
                    auto job = std::format("job-{}-{}", id, n);
                    const auto start = std::chrono::steady_clock::now();
                    if (!queue.push(job)) return;
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start);
                    log("producer {} -> {}   (blocked {} ms)", id, job, waited.count());
                }
            });
        }
    }  // producers finished

    log("main: all jobs submitted, closing queue (consumers will drain what is left)");
    queue.close();
    consumers.clear();  // joins the consumers
    log("main: done, queue empty = {}", queue.empty());
}
