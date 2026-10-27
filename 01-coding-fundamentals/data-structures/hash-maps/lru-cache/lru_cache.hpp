#pragma once
// LruCache<Key, Value> — a fixed-capacity cache that evicts the Least Recently Used entry.
//
//   get / put / erase / contains: O(1) average time.
//
// Data structures (see README.md for the full walkthrough):
//   std::list<Entry>                              recency order, most recent at the front
//   std::unordered_map<Key, std::list::iterator>  key -> node in that list
//
// The list gives O(1) "move to front" (splice) and O(1) "remove oldest" (back);
// the hash map gives O(1) "find the node for this key".
//
// Not thread-safe by design: wrap it in a mutex, or see the concurrent
// expiring-cache exercise in 04-system-design/databases-caching.

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace roadmap {

template <typename Key, typename Value, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>>
class LruCache {
public:
    using key_type = Key;
    using mapped_type = Value;
    // Stored as a non-const pair so eviction can MOVE the key out. Callers only ever see the
    // entries through const_iterator, so they still cannot change a key behind the index's back.
    using entry_type = std::pair<Key, Value>;
    using const_iterator = typename std::list<entry_type>::const_iterator;

    explicit LruCache(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) throw std::invalid_argument("LruCache capacity must be greater than zero");
        index_.reserve(capacity_);  // avoid rehashing (and its latency spikes) as the cache fills
    }

    /// Looks up `key`. On a hit, the entry becomes the most recently used.
    /// Returns a pointer to the cached value (valid until that entry is evicted or erased),
    /// or nullptr on a miss.
    ///
    /// Supports heterogeneous lookup (e.g. std::string_view for std::string keys) when the
    /// Hash and KeyEqual types are transparent — no temporary Key is constructed.
    template <typename K>
    [[nodiscard]] Value* get(const K& key) {
        auto found = index_.find(key);
        if (found == index_.end()) return nullptr;
        touch(found->second);
        return &found->second->second;
    }

    /// Looks up `key` WITHOUT changing its recency. Useful for debugging and tests.
    template <typename K>
    [[nodiscard]] const Value* peek(const K& key) const {
        auto found = index_.find(key);
        return found == index_.end() ? nullptr : &found->second->second;
    }

    template <typename K>
    [[nodiscard]] bool contains(const K& key) const {
        return index_.contains(key);
    }

    /// Inserts or updates `key`. The entry becomes the most recently used.
    /// If inserting a NEW key into a full cache, the least recently used entry is evicted
    /// and returned so the caller can react (write-back, metrics, logging...).
    template <typename K, typename V>
    std::optional<std::pair<Key, Value>> put(K&& key, V&& value) {
        if (auto found = index_.find(key); found != index_.end()) {
            found->second->second = std::forward<V>(value);  // update in place
            touch(found->second);
            return std::nullopt;
        }

        std::optional<std::pair<Key, Value>> evicted;
        if (entries_.size() == capacity_) evicted = evict_oldest();

        entries_.emplace_front(std::forward<K>(key), std::forward<V>(value));
        try {
            index_.emplace(entries_.front().first, entries_.begin());
        } catch (...) {
            entries_.pop_front();  // keep list and map consistent if the map allocation throws
            throw;
        }
        return evicted;
    }

    /// Removes `key` if present. Returns true if something was removed.
    template <typename K>
    bool erase(const K& key) {
        auto found = index_.find(key);
        if (found == index_.end()) return false;
        entries_.erase(found->second);
        index_.erase(found);
        return true;
    }

    void clear() noexcept {
        index_.clear();
        entries_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// Iterates from most recently used to least recently used.
    [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

private:
    using list_iterator = typename std::list<entry_type>::iterator;

    // Move a node to the front in O(1). splice relinks pointers: no allocation, no copy,
    // and every iterator stored in index_ stays valid.
    void touch(list_iterator node) noexcept { entries_.splice(entries_.begin(), entries_, node); }

    std::pair<Key, Value> evict_oldest() {
        auto& oldest = entries_.back();
        index_.erase(oldest.first);  // erase from the map first, while the key is still intact
        std::pair<Key, Value> victim(std::move(oldest));
        entries_.pop_back();
        return victim;
    }

    std::size_t capacity_;
    std::list<entry_type> entries_;  // front = most recently used, back = least recently used
    std::unordered_map<Key, list_iterator, Hash, KeyEqual> index_;
};

/// Transparent hasher for string keys: lets LruCache<std::string, V, StringHash, std::equal_to<>>
/// be queried with std::string_view or const char* without allocating a std::string.
struct StringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

}  // namespace roadmap
