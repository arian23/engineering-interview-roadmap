// Demo: an LRU cache in front of a "slow database", printing the recency order after every step.

#include "lru_cache.hpp"

#include <print>
#include <string>
#include <string_view>

namespace {

using Cache = roadmap::LruCache<std::string, std::string, roadmap::StringHash, std::equal_to<>>;

void show(const Cache& cache) {
    std::print("    cache (most -> least recent):");
    for (const auto& [key, value] : cache) std::print(" [{}={}]", key, value);
    std::println("");
}

std::string slow_database_lookup(std::string_view user) { return std::string(user) + "@example.com"; }

std::string fetch_email(Cache& cache, std::string_view user) {
    if (const std::string* hit = cache.get(user)) {  // string_view lookup: no allocation
        std::println("get {:<6} HIT", user);
        return *hit;
    }
    std::println("get {:<6} MISS -> loading from database", user);
    std::string email = slow_database_lookup(user);
    if (auto evicted = cache.put(std::string(user), email)) {
        std::println("    evicted {} (least recently used)", evicted->first);
    }
    return email;
}

}  // namespace

int main() {
    Cache cache(3);
    for (std::string_view user : {"alice", "bob", "carol", "alice", "dave", "bob", "alice", "erin"}) {
        (void)fetch_email(cache, user);
        show(cache);
    }
}
