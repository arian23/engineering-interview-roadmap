#include "lru_cache.hpp"
#include "minitest.hpp"

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using roadmap::LruCache;

namespace {
template <typename Cache>
std::vector<typename Cache::key_type> keys_mru_to_lru(const Cache& cache) {
    std::vector<typename Cache::key_type> keys;
    for (const auto& [key, value] : cache) keys.push_back(key);
    return keys;
}
}  // namespace

TEST(rejects_zero_capacity) { CHECK_THROWS((LruCache<int, int>(0)), std::invalid_argument); }

TEST(miss_returns_nullptr) {
    LruCache<int, std::string> cache(2);
    CHECK(cache.get(1) == nullptr);
    CHECK(!cache.contains(1));
}

TEST(put_then_get) {
    LruCache<int, std::string> cache(2);
    cache.put(1, "one");
    CHECK(cache.get(1) != nullptr);
    CHECK_EQ(*cache.get(1), std::string("one"));
    CHECK_EQ(cache.size(), 1u);
}

TEST(evicts_least_recently_used) {
    LruCache<int, std::string> cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    auto evicted = cache.put(3, "three");  // 1 is the oldest
    CHECK(evicted.has_value());
    CHECK_EQ(evicted->first, 1);
    CHECK_EQ(evicted->second, std::string("one"));
    CHECK(!cache.contains(1));
    CHECK(cache.contains(2));
    CHECK(cache.contains(3));
}

TEST(get_refreshes_recency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    (void)cache.get(1);              // 1 becomes most recent, so 2 is now the oldest
    auto evicted = cache.put(3, 30);
    CHECK_EQ(evicted->first, 2);
    CHECK(cache.contains(1));
}

TEST(peek_does_not_refresh_recency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK_EQ(*cache.peek(1), 10);  // does NOT touch 1
    auto evicted = cache.put(3, 30);
    CHECK_EQ(evicted->first, 1);
}

TEST(updating_existing_key_does_not_evict_and_refreshes) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK(!cache.put(1, 11).has_value());  // update, no eviction
    CHECK_EQ(cache.size(), 2u);
    CHECK_EQ(*cache.peek(1), 11);
    CHECK_EQ(cache.put(3, 30)->first, 2);  // 2 was the oldest after the update
}

TEST(iteration_is_most_to_least_recent) {
    LruCache<int, int> cache(3);
    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);
    (void)cache.get(1);
    CHECK((keys_mru_to_lru(cache) == std::vector<int>{1, 3, 2}));
}

TEST(erase_and_clear) {
    LruCache<int, int> cache(2);
    cache.put(1, 1);
    cache.put(2, 2);
    CHECK(cache.erase(1));
    CHECK(!cache.erase(1));
    CHECK_EQ(cache.size(), 1u);
    cache.put(3, 3);
    CHECK_EQ(cache.put(4, 4)->first, 2);  // full again: 4 evicts the oldest, which is 2
    cache.clear();
    CHECK(cache.empty());
}

TEST(supports_move_only_values) {
    LruCache<int, std::unique_ptr<int>> cache(1);
    cache.put(1, std::make_unique<int>(7));
    CHECK_EQ(**cache.get(1), 7);
    auto evicted = cache.put(2, std::make_unique<int>(8));
    CHECK(evicted.has_value());
    CHECK_EQ(*evicted->second, 7);  // ownership handed back to the caller
}

TEST(heterogeneous_lookup_with_string_view) {
    LruCache<std::string, int, roadmap::StringHash, std::equal_to<>> cache(2);
    cache.put(std::string("alpha"), 1);
    std::string_view key = "alpha";
    CHECK(cache.get(key) != nullptr);  // no std::string is constructed for the lookup
    CHECK(cache.contains("alpha"));
    CHECK(cache.erase(key));
}

TEST(randomized_against_reference_model) {
    // Reference model: a vector ordered MRU -> LRU, searched linearly. Obviously correct, O(n).
    constexpr std::size_t capacity = 8;
    LruCache<int, int> cache(capacity);
    std::vector<std::pair<int, int>> model;

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> key_dist(0, 20);
    std::uniform_int_distribution<int> op_dist(0, 2);

    for (int step = 0; step < 20'000; ++step) {
        const int key = key_dist(rng);
        auto it = std::ranges::find(model, key, &std::pair<int, int>::first);
        switch (op_dist(rng)) {
            case 0: {  // get
                int* got = cache.get(key);
                CHECK_EQ(got != nullptr, it != model.end());
                if (it != model.end()) {
                    CHECK_EQ(*got, it->second);
                    std::rotate(model.begin(), it, it + 1);  // move to front
                }
                break;
            }
            case 1: {  // put
                const int value = step;
                auto evicted = cache.put(key, value);
                if (it != model.end()) {
                    it->second = value;
                    std::rotate(model.begin(), it, it + 1);
                    CHECK(!evicted.has_value());
                } else {
                    if (model.size() == capacity) {
                        CHECK(evicted.has_value());
                        CHECK_EQ(evicted->first, model.back().first);
                        model.pop_back();
                    } else {
                        CHECK(!evicted.has_value());
                    }
                    model.insert(model.begin(), {key, value});
                }
                break;
            }
            default: {  // erase
                CHECK_EQ(cache.erase(key), it != model.end());
                if (it != model.end()) model.erase(it);
            }
        }
        CHECK_EQ(cache.size(), model.size());
    }
    std::vector<int> expected;
    for (const auto& [k, v] : model) expected.push_back(k);
    CHECK(keys_mru_to_lru(cache) == expected);
}

int main() { return minitest::run_all(); }
