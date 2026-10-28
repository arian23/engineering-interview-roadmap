// Producer-Consumer (C++23)
//
// Idea: producers create work items and put them into a shared BUFFER; consumers take items out
// and process them. The buffer decouples the two sides: they run at their own speed, on their own
// threads, and neither needs to know about the other. Theory: ../README.md#producer-consumer
//
// The buffer must solve three problems:
//   * Mutual exclusion   two threads must never modify the buffer at the same time
//   * Empty buffer       a consumer must WAIT (not spin, not fail) until an item arrives
//   * Full buffer        a producer must WAIT until there is space (this bound limits memory)
//
// Two classic implementations:
//   1. Dijkstra's bounded buffer: a ring buffer + two counting semaphores (free slots, filled slots)
//      + a mutex. This is the textbook solution and maps directly onto the three problems above.
//   2. A closable channel (as in Go): a deque + mutex + two condition variables + close().
//      close() gives a clean way to tell consumers "no more items" instead of sending a
//      "poison pill" item to each consumer.
// For overload policies (drop, timeout) and pipelines, see
// 05-system-programming/concurrency-synchronization/producer-consumer-backpressure.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <numeric>
#include <optional>
#include <print>
#include <semaphore>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ===================================================================== 1. semaphore ring buffer

template <typename T, std::size_t N>
class RingBuffer {
public:
    void put(T item) {
        free_slots_.acquire();  // wait for an empty slot (blocks while the buffer is full)
        {
            std::lock_guard lock(mutex_);  // only one thread touches the indices at a time
            slots_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % N;
        }
        filled_slots_.release();  // one more item available: wakes a waiting consumer
    }

    T take() {
        filled_slots_.acquire();  // wait for an item (blocks while the buffer is empty)
        T item;
        {
            std::lock_guard lock(mutex_);
            item = std::move(slots_[head_]);
            head_ = (head_ + 1) % N;
        }
        free_slots_.release();  // one more free slot: wakes a waiting producer
        return item;
    }

private:
    std::array<T, N> slots_{};
    std::size_t head_ = 0, tail_ = 0;
    std::mutex mutex_;
    std::counting_semaphore<N> free_slots_{N};  // starts at N: every slot is free
    std::counting_semaphore<N> filled_slots_{0};  // starts at 0: nothing to consume
};

// ===================================================================== 2. closable channel

template <typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity) : capacity_(capacity) {}

    /// Blocks while full. Returns false if the channel was closed.
    bool send(T item) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    /// Blocks while empty. Returns nullopt only when closed AND every item has been received.
    std::optional<T> receive() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();  // wake every consumer so each can see "closed and empty"
        not_full_.notify_all();
    }

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> items_;
    bool closed_ = false;
};

// ===================================================================== demo

int main() {
    constexpr int kProducers = 3, kConsumers = 2, kItemsEach = 1000;
    constexpr long long kExpectedSum = static_cast<long long>(kProducers) * kItemsEach * (kItemsEach + 1) / 2;

    std::println("1) Semaphore ring buffer (capacity 4): {} producers, {} consumers, poison pills to stop", kProducers, kConsumers);
    {
        RingBuffer<int, 4> buffer;
        std::atomic<long long> sum{0};
        std::array<int, kConsumers> handled{};
        {
            std::vector<std::jthread> consumers;
            for (int c = 0; c < kConsumers; ++c) {
                consumers.emplace_back([&, c] {
                    for (;;) {
                        const int item = buffer.take();
                        if (item < 0) return;  // poison pill: a special value meaning "stop"
                        sum += item;
                        ++handled[static_cast<std::size_t>(c)];
                    }
                });
            }
            {
                std::vector<std::jthread> producers;
                for (int p = 0; p < kProducers; ++p) {
                    producers.emplace_back([&] {
                        for (int i = 1; i <= kItemsEach; ++i) buffer.put(i);
                    });
                }
            }
            for (int c = 0; c < kConsumers; ++c) buffer.put(-1);  // one pill PER consumer
        }
        std::println("   sum {} (expected {}), items per consumer: {} / {}", sum.load(), kExpectedSum, handled[0], handled[1]);
    }

    std::println("\n2) Closable channel (capacity 4): producers finish, close() ends the consumers cleanly");
    {
        Channel<int> channel(4);
        std::atomic<long long> sum{0};
        std::atomic<int> max_waiting_ms{0};
        {
            std::vector<std::jthread> consumers;
            for (int c = 0; c < kConsumers; ++c) {
                consumers.emplace_back([&] {
                    while (auto item = channel.receive()) {
                        sum += *item;
                        if (*item % 250 == 0) std::this_thread::sleep_for(2ms);  // occasional slow item
                    }
                });
            }
            {
                std::vector<std::jthread> producers;
                for (int p = 0; p < kProducers; ++p) {
                    producers.emplace_back([&] {
                        for (int i = 1; i <= kItemsEach; ++i) {
                            const auto start = std::chrono::steady_clock::now();
                            channel.send(i);
                            const int waited = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                    std::chrono::steady_clock::now() - start).count());
                            if (waited > max_waiting_ms) max_waiting_ms = waited;
                        }
                    });
                }
            }
            channel.close();
        }
        std::println("   sum {} (expected {}); producers waited up to {} ms when the buffer was full",
                     sum.load(), kExpectedSum, max_waiting_ms.load());
        std::println("   send() after close() returns {}", channel.send(1));
    }
}
