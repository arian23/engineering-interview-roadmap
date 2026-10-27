#pragma once
// BoundedQueue<T> — a thread-safe, fixed-capacity, multi-producer / multi-consumer FIFO queue.
//
// Guarantees
//   * At most `capacity()` items are ever stored (memory is bounded).
//   * Producers block while the queue is full; consumers block while it is empty.
//   * close() starts a graceful shutdown: new pushes fail, blocked threads wake up,
//     and consumers keep receiving the items that were already queued until it is empty.
//   * Every blocking call has a cancellable (std::stop_token) and a timed variant.
//   * A failed push never consumes the caller's value (useful for move-only types).
//
// See README.md for a detailed walkthrough of the design.

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace roadmap {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be greater than zero");
        }
    }

    // The queue owns a mutex and condition variables, so it can be neither copied nor moved.
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // ------------------------------------------------------------------ producers

    /// Blocks while the queue is full.
    /// Returns false (and leaves `value` untouched) if the queue is or becomes closed.
    template <typename U = T>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool push(U&& value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        return enqueue_and_notify(lock, std::forward<U>(value));
    }

    /// Like push(), but also gives up (returns false) when `stop` is requested.
    template <typename U = T>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool push(U&& value, std::stop_token stop) {
        std::unique_lock lock(mutex_);
        if (!not_full_.wait(lock, stop, [this] { return closed_ || items_.size() < capacity_; })) {
            return false;  // stop was requested while the queue was still full
        }
        return enqueue_and_notify(lock, std::forward<U>(value));
    }

    /// Like push(), but gives up (returns false) after `timeout`.
    template <typename U = T, typename Rep, typename Period>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool push_for(U&& value, std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        if (!not_full_.wait_for(lock, timeout, [this] { return closed_ || items_.size() < capacity_; })) {
            return false;  // timed out while the queue was still full
        }
        return enqueue_and_notify(lock, std::forward<U>(value));
    }

    /// Never blocks. Returns false if the queue is full or closed.
    template <typename U = T>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool try_push(U&& value) {
        std::unique_lock lock(mutex_);
        if (items_.size() >= capacity_) return false;
        return enqueue_and_notify(lock, std::forward<U>(value));
    }

    // ------------------------------------------------------------------ consumers

    /// Blocks while the queue is empty.
    /// Returns std::nullopt only when the queue is closed AND fully drained.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        return dequeue_and_notify(lock);
    }

    /// Like pop(), but also returns std::nullopt when `stop` is requested.
    [[nodiscard]] std::optional<T> pop(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, stop, [this] { return closed_ || !items_.empty(); });
        return dequeue_and_notify(lock);  // empty here means stopped, or closed and drained
    }

    /// Like pop(), but returns std::nullopt after `timeout`.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        not_empty_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
        return dequeue_and_notify(lock);
    }

    /// Never blocks. Returns std::nullopt if the queue is empty.
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock lock(mutex_);
        return dequeue_and_notify(lock);
    }

    // ------------------------------------------------------------------ lifecycle

    /// Graceful shutdown. Idempotent and safe to call from any thread.
    void close() {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            closed_ = true;
        }
        // Wake *every* waiter: each one re-checks its predicate and sees closed_ == true.
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    // ------------------------------------------------------------------ observers
    // These are snapshots: in a concurrent program the value may change right after it is read.

    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // Precondition: `lock` holds mutex_ and there is room (or the queue is closed).
    template <typename U>
    bool enqueue_and_notify(std::unique_lock<std::mutex>& lock, U&& value) {
        if (closed_) return false;                  // value is not touched on failure
        items_.emplace_back(std::forward<U>(value));  // strong guarantee: on throw, nothing changed
        lock.unlock();                                // let the woken consumer acquire the lock immediately
        not_empty_.notify_one();
        return true;
    }

    // Precondition: `lock` holds mutex_.
    std::optional<T> dequeue_and_notify(std::unique_lock<std::mutex>& lock) {
        if (items_.empty()) return std::nullopt;
        std::optional<T> value(std::move(items_.front()));  // if this throws, the item stays queued
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;                // guards items_ and closed_
    std::condition_variable_any not_full_;    // signalled when space frees up (or on close)
    std::condition_variable_any not_empty_;   // signalled when an item arrives (or on close)
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace roadmap
