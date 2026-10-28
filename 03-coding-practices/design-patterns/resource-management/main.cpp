// Memory and resource-management design (C++23)
//
// Idea: decide, for every resource, WHO owns it, HOW LONG it lives and WHERE its memory comes from,
// and encode those decisions in types instead of comments. Theory:
// ../README.md#memory-and-resource-management-design
//
// Sections:
//   1. Ownership in types: unique_ptr (one owner), shared_ptr (shared), raw pointer / reference (no ownership)
//   2. Reference cycles: two shared_ptrs pointing at each other leak; weak_ptr breaks the cycle
//   3. Object pool: reuse expensive objects instead of creating and destroying them; a custom deleter
//      returns them to the pool automatically (RAII again)
//   4. Arena (region) allocation with std::pmr: allocate from one big block, free everything at once
//   5. Measuring: a counting std::pmr::memory_resource shows how many allocations reach the heap

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <print>
#include <string>
#include <vector>

// ===================================================================== allocation counting (for the demo)

// A memory_resource that forwards to an "upstream" resource and counts what passes through it.
// Chaining resources like this is how std::pmr is meant to be used (statistics, limits, logging...).
class CountingResource final : public std::pmr::memory_resource {
public:
    explicit CountingResource(std::pmr::memory_resource* upstream) : upstream_(upstream) {}
    [[nodiscard]] std::size_t allocations() const noexcept { return allocations_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        bytes_ += bytes;
        return upstream_->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    std::pmr::memory_resource* upstream_;
    std::size_t allocations_ = 0;
    std::size_t bytes_ = 0;
};

// ===================================================================== 1. ownership

struct Texture {
    explicit Texture(std::string n) : name(std::move(n)) { std::println("     + Texture {}", name); }
    ~Texture() { std::println("     - Texture {}", name); }
    std::string name;
};

class Renderer {
public:
    // Takes ownership: the caller's unique_ptr is moved in and becomes empty.
    void adopt(std::unique_ptr<Texture> t) { owned_.push_back(std::move(t)); }
    // Shares ownership: the texture lives while anyone still holds it.
    void share(std::shared_ptr<Texture> t) { shared_.push_back(std::move(t)); }
    // Only uses it: a reference says "I do not own this and will not keep it".
    static void draw(const Texture& t) { std::println("     draw {}", t.name); }

private:
    std::vector<std::unique_ptr<Texture>> owned_;
    std::vector<std::shared_ptr<Texture>> shared_;
};

// ===================================================================== 2. cycles

struct Employee;
struct Team {
    explicit Team(std::string n) : name(std::move(n)) {}
    ~Team() { std::println("     - Team {} destroyed", name); }
    std::string name;
    std::vector<std::shared_ptr<Employee>> members;  // the team owns its members
};
struct Employee {
    explicit Employee(std::string n) : name(std::move(n)) {}
    ~Employee() { std::println("     - Employee {} destroyed", name); }
    std::string name;
    std::shared_ptr<Team> team_strong;  // BAD: member owns the team too -> cycle
    std::weak_ptr<Team> team;           // GOOD: a non-owning back-pointer
};

// ===================================================================== 3. object pool

struct Connection {  // pretend this is expensive to create (TCP + TLS handshake)
    explicit Connection(int i) : id(i) { ++constructed; }
    int id;
    int uses = 0;
    static inline int constructed = 0;
};

class ConnectionPool {
public:
    // A unique_ptr whose deleter RETURNS the object to the pool instead of deleting it.
    struct Returner {
        ConnectionPool* pool;
        void operator()(Connection* c) const { pool->give_back(c); }
    };
    using Lease = std::unique_ptr<Connection, Returner>;

    explicit ConnectionPool(std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) idle_.push_back(std::make_unique<Connection>(static_cast<int>(i)));
        created_ = size;
    }

    [[nodiscard]] Lease acquire() {
        std::lock_guard lock(mutex_);
        if (idle_.empty()) {  // grow on demand (a real pool might block or fail instead)
            idle_.push_back(std::make_unique<Connection>(static_cast<int>(created_++)));
        }
        Connection* c = idle_.back().release();
        idle_.pop_back();
        ++c->uses;
        return Lease(c, Returner{this});
    }

    [[nodiscard]] std::size_t created() const { return created_; }
    [[nodiscard]] std::size_t idle() const {
        std::lock_guard lock(mutex_);
        return idle_.size();
    }

private:
    void give_back(Connection* c) {
        std::lock_guard lock(mutex_);
        idle_.emplace_back(c);
    }
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Connection>> idle_;
    std::size_t created_ = 0;
};

// ===================================================================== 4. arena allocation

struct Node {
    int value;
    Node* next;
};

// Builds a linked list of n nodes using whatever memory resource it is given.
Node* build_list(std::pmr::memory_resource* mr, int n) {
    std::pmr::polymorphic_allocator<Node> alloc(mr);
    Node* head = nullptr;
    for (int i = 0; i < n; ++i) {
        Node* node = alloc.allocate(1);
        alloc.construct(node, Node{i, head});
        head = node;
    }
    return head;
}

void free_list(std::pmr::memory_resource* mr, Node* head) {  // needed only for the general-purpose heap
    std::pmr::polymorphic_allocator<Node> alloc(mr);
    while (head) {
        Node* next = head->next;
        alloc.deallocate(head, 1);
        head = next;
    }
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// ===================================================================== demo

int main() {
    std::println("1) Ownership expressed in types");
    {
        Renderer renderer;
        auto background = std::make_unique<Texture>("background");
        Renderer::draw(*background);
        renderer.adopt(std::move(background));  // ownership transferred
        std::println("     after adopt, caller's pointer is {}", background ? "set" : "empty");
        auto font = std::make_shared<Texture>("font");
        renderer.share(font);
        std::println("     font use_count = {} (caller + renderer)", font.use_count());
        font.reset();
        std::println("     caller released font; renderer still keeps it alive");
        std::println("     renderer goes out of scope:");
    }

    std::println("\n2) Reference cycle: shared_ptr both ways leaks, weak_ptr back-pointer does not");
    {
        auto team = std::make_shared<Team>("leaky");
        auto alice = std::make_shared<Employee>("alice");
        alice->team_strong = team;  // team -> alice and alice -> team: each keeps the other alive
        team->members.push_back(alice);
        std::println("     leaving the scope... (no destructor messages will appear: memory leaked)");
    }
    {
        auto team = std::make_shared<Team>("fixed");
        auto bob = std::make_shared<Employee>("bob");
        bob->team = team;  // weak: does not keep the team alive
        team->members.push_back(bob);
        if (auto t = bob->team.lock()) std::println("     bob's team (via weak_ptr::lock): {}", t->name);
        std::println("     leaving the scope:");
    }

    std::println("\n3) Object pool: 1,000 requests, each leasing 2 connections, from a pool of 4");
    {
        ConnectionPool pool(4);
        const int before = Connection::constructed;
        for (int request = 0; request < 1000; ++request) {
            auto a = pool.acquire();  // returned to the pool automatically at the end of each iteration
            auto b = pool.acquire();
            (void)a, (void)b;
        }
        std::println("     connections constructed during the 2,000 leases: {}   total ever created: {}   idle now: {}",
                     Connection::constructed - before, pool.created(), pool.idle());
        std::println("     (without a pool: 2,000 connections created and destroyed)");
    }

    std::println("\n4) Arena allocation with std::pmr: a linked list of 200,000 nodes");
    constexpr int kNodes = 200'000;
    {
        CountingResource heap(std::pmr::new_delete_resource());
        const auto t = std::chrono::steady_clock::now();
        Node* list = build_list(&heap, kNodes);
        free_list(&heap, list);
        std::println("     general heap:  {:>7} heap allocations  {:6.2f} ms  (one allocate and one free per node)",
                     heap.allocations(), ms_since(t));
    }
    {
        CountingResource heap(std::pmr::new_delete_resource());
        const auto t = std::chrono::steady_clock::now();
        {
            std::pmr::monotonic_buffer_resource arena(&heap);  // asks `heap` only for large, growing chunks
            Node* list = build_list(&arena, kNodes);
            (void)list;  // no per-node free
        }  // the arena hands all its chunks back here, in one go
        std::println("     arena:         {:>7} heap allocations  {:6.2f} ms  ({:.1f} MB in a few chunks, freed together)",
                     heap.allocations(), ms_since(t), static_cast<double>(heap.bytes()) / 1e6);
    }
    {
        alignas(std::max_align_t) static std::byte buffer[4096];
        CountingResource heap(std::pmr::null_memory_resource());  // reaching the heap would throw bad_alloc
        std::pmr::monotonic_buffer_resource arena(buffer, sizeof buffer, &heap);
        std::pmr::vector<int> small(&arena);
        for (int i = 0; i < 100; ++i) small.push_back(i);
        std::println("     fixed buffer:  {:>7} heap allocations  for a pmr::vector of {} ints (all memory came from a 4 KB static buffer)",
                     heap.allocations(), small.size());
    }
}
