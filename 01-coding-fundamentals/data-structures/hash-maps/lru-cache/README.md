# LRU cache with O(1) lookup and update

**What it tests:** hash maps, linked lists, complexity.

A fixed-capacity cache. When it is full and a new key arrives, it evicts the entry
that was **used least recently**. `get`, `put` and `erase` all run in **O(1)
average time**.

| File | Purpose |
|---|---|
| [`lru_cache.hpp`](lru_cache.hpp) | The implementation (header-only, C++23) |
| [`test.cpp`](test.cpp) | 12 tests, including 20,000 random operations checked against a reference model |
| [`main.cpp`](main.cpp) | Demo: a cache in front of a slow "database", showing the recency order |

## API

```cpp
roadmap::LruCache<int, std::string> cache(100);

cache.put(42, "answer");                  // insert or update; becomes most recent
if (std::string* v = cache.get(42)) ...   // hit -> pointer, miss -> nullptr; refreshes recency
const std::string* v = cache.peek(42);    // look without refreshing recency
auto evicted = cache.put(7, "x");         // optional<pair<Key, Value>>: what got evicted, if anything
cache.erase(42);  cache.contains(42);  cache.size();  cache.clear();
for (const auto& [key, value] : cache)    // iterates most recent -> least recent
```

## The idea: two data structures working together

Neither structure can do everything in O(1) on its own:

| Need | Hash map alone | Linked list alone |
|---|---|---|
| Find a key | O(1) | O(n) scan |
| Move an entry to "most recent" | no notion of order | O(1) if you have the node |
| Remove the least recent entry | no notion of order | O(1): it's the tail |

So we use **both**, and each one covers what the other can't do:

```
 index_  (unordered_map<Key, list::iterator>)
 ┌──────┬──────┐
 │ "bob"│  ●───┼──────────────┐
 │"alice│  ●───┼────┐         │
 │"dave"│  ●───┼─┐  │         │
 └──────┴──────┘ │  │         │
                 ▼  ▼         ▼
 entries_  front ⇄ [dave] ⇄ [alice] ⇄ [bob] ⇄ back
 (std::list)     most recent           least recent  ← evict from here
```

- **`entries_` (`std::list<std::pair<Key, Value>>`)** keeps the entries in **recency
  order**. The front is most recently used; the back is the next to evict.
- **`index_` (`std::unordered_map<Key, list::iterator>`)** maps each key directly to
  **its node in the list**, so we never have to search the list.

### Why `std::list` specifically

Two `std::list` properties make this work:

1. **`splice` is O(1).** `entries_.splice(entries_.begin(), entries_, node)` relinks a
   node to the front by changing a few pointers. Nothing is copied or allocated.
2. **Iterators stay valid.** A list iterator remains valid until *that node* is erased,
   even after other insertions, erasures and splices. That's why it's safe to store
   iterators in the hash map. A `std::vector` or `std::deque` would invalidate them on
   insertion or erasure.

## Each operation step by step

### `get(key)`

```
1. it = index_.find(key)                   O(1) average
2. miss -> return nullptr
3. splice node to the front of entries_    O(1)   (mark as most recently used)
4. return &node->value
```

### `put(key, value)`

```
1. it = index_.find(key)
2. key exists?  -> overwrite value, splice to front, done (no eviction)
3. cache full?  -> victim = entries_.back()
                   index_.erase(victim.key)       (erase map entry first, while the key is intact)
                   move victim out, entries_.pop_back()
4. entries_.emplace_front(key, value)
5. index_.emplace(key, entries_.begin())
6. return the victim (if any) to the caller
```

**Exception safety:** if step 5 throws (the hash map failed to allocate), we undo step
4 with `pop_front()`, so the list and map never disagree.

### `erase(key)`

Find in the map, erase the list node it points to, then erase the map entry. Both are
O(1).

## Complexity

| Operation | Average | Worst case |
|---|---|---|
| `get`, `peek`, `contains` | O(1) | O(n), if every key hashes to the same bucket |
| `put` (including eviction) | O(1) | O(n), same reason |
| `erase` | O(1) | O(n) |
| Memory | O(capacity) | one list node + one map entry per key |

"Average O(1)" comes from the hash map: with a good hash function, lookups take
constant time *on average*. A very poor hash function (or an attacker who chooses keys
that collide) can make every operation linear. The constructor calls
`index_.reserve(capacity)`, so the map never rehashes as it fills. That avoids a
sudden O(n) pause while the cache is in use.

## Modern C++ details

- **Heterogeneous lookup (C++20).** With a *transparent* hash (`StringHash` declares
  `is_transparent`) and `std::equal_to<>`, you can look up a `std::string` key using a
  `std::string_view` or `const char*`, without building a temporary `std::string`:
  ```cpp
  roadmap::LruCache<std::string, User, roadmap::StringHash, std::equal_to<>> users(1000);
  users.get(std::string_view{"alice"});   // no allocation on the lookup path
  ```
- **Perfect forwarding** in `put(K&&, V&&)`, so move-only values like `std::unique_ptr`
  work. Evicted values are *moved back to the caller* instead of destroyed.
- **`std::optional<std::pair<Key, Value>>`** as the eviction result, so the caller can
  write back dirty data or record metrics.
- **`contains()`** (C++20) and **`std::ranges::find` with a projection** in the tests.

## Design decisions and trade-offs

- **`get` returns a pointer, not a copy.** That avoids copying large values. The
  pointer is valid until that entry is evicted or erased; don't hold on to it across
  `put` calls.
- **The key is stored twice**, once in the list and once in the map. That's simple and
  fast. For very large keys you could store the key only in the list and keep a
  `std::reference_wrapper` in the map, which complicates hashing and erasure.
- **Not thread-safe.** The *simplest* way to share it is to put a mutex around every
  call. Note that **`get` modifies the list**, so even reads need an exclusive lock;
  a reader–writer lock doesn't help. At scale you split the cache into N shards, each
  with its own mutex. See the concurrent expiring cache exercise.

## Tests

| Test | What it proves |
|---|---|
| `evicts_least_recently_used` | The oldest entry goes first, and is returned to the caller |
| `get_refreshes_recency` / `peek_does_not_refresh_recency` | Only `get` counts as a "use" |
| `updating_existing_key_does_not_evict_and_refreshes` | An update is a use, never an eviction |
| `iteration_is_most_to_least_recent` | The list order is exactly the recency order |
| `supports_move_only_values` | Works with `unique_ptr`; ownership comes back on eviction |
| `heterogeneous_lookup_with_string_view` | Allocation-free lookups |
| `randomized_against_reference_model` | 20,000 random get/put/erase operations agree with a simple O(n) model at every step |

## Build and run

```bash
cmake --build build --target lru_cache_test lru_cache_demo
./build/01-coding-fundamentals/data-structures/hash-maps/lru-cache/lru_cache_test
./build/01-coding-fundamentals/data-structures/hash-maps/lru-cache/lru_cache_demo
```

## Interview follow-ups

- **LFU (least *frequently* used)?** Keep one list per frequency plus a `min_freq`
  pointer. That's still O(1).
- **Why not `std::map` (a tree)?** Lookups would be O(log n), and a tree has no
  recency order anyway.
- **Approximate LRU at scale:** Redis samples a few random keys and evicts the oldest
  among them. CLOCK and second-chance algorithms avoid moving a node on every hit,
  which helps under heavy concurrency.
- **TTL as well as LRU:** combine the recency list with an expiry check on read,
  plus a background sweep (see the expiring cache exercise).
