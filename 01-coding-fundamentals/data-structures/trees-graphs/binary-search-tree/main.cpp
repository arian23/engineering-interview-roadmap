// =====================================================================================================
// Binary Search Tree (C++23)
// =====================================================================================================
//
// WHAT A BST IS
//   A binary tree (each node has at most two children) that keeps one rule, the BST INVARIANT:
//
//       every key in a node's LEFT subtree  <  the node's key  <  every key in its RIGHT subtree
//
//   The rule holds at every node, not just between a parent and its children. It means a search can
//   throw away half of the remaining tree at each step, just like binary search on a sorted array, but
//   unlike an array a BST can insert and delete without shifting elements.
//
//               50
//             /    \          a valid BST
//           30      70          in-order traversal visits keys in SORTED order:
//          /  \    /  \         20 30 40 50 60 70 80
//        20   40  60   80
//
// OPERATIONS AND HOW THEY WORK (h = height of the tree)
//   insert(k)         start at the root; go left if k < node, right if k > node; attach a new leaf
//                     at the empty spot you reach. Duplicate keys are rejected (this is a set).   O(h)
//   contains(k)       the same walk; stop when you find k or fall off the tree.                    O(h)
//   min() / max()     keep going left / right until there is no child.                             O(h)
//   erase(k)          three cases (see erase_node below):                                          O(h)
//                       1. leaf             -> just remove it
//                       2. one child        -> replace the node by its child
//                       3. two children     -> copy in the in-order SUCCESSOR (smallest key of the
//                                              right subtree), then delete that successor, which has
//                                              at most one child, so it is case 1 or 2
//   floor(k)          largest key <= k: when going right, remember the node as a candidate       O(h)
//   ceiling(k)        smallest key >= k: the mirror image                                         O(h)
//   successor(k)      smallest key > k  (next key in sorted order)                                 O(h)
//   predecessor(k)    largest key < k                                                              O(h)
//   kth_smallest(k)   in-order walk that stops after k nodes                                    O(h + k)
//   range(lo, hi)     in-order walk that skips subtrees entirely outside [lo, hi]           O(h + output)
//   height(), size(), is_valid()                                                                   O(n)
//   traversals        in-order (sorted), pre-order (copy / serialise a tree), post-order (delete a
//                     tree: children before parent), level-order (BFS with a queue, level by level)
//
// THE CATCH: HEIGHT
//   Everything is O(h). With random insertion order h is about 2·ln(n) on average, so ~ O(log n):
//   1,000,000 keys -> height around 50. But inserting keys IN SORTED ORDER makes every new key the
//   right child of the previous one: the tree becomes a linked list, h = n, and every operation is O(n).
//   The demo builds both and prints their heights. Self-balancing trees (AVL, red-black) do extra
//   rotations on insert/erase to keep h = O(log n) always; std::map and std::set are red-black trees.
//
// IMPLEMENTATION CHOICES
//   * Nodes are owned by std::unique_ptr: parent owns children, the tree owns the root. No manual
//     delete anywhere, and erase/clear cannot leak (RAII).
//   * insert, contains, erase, floor, ceiling, successor, kth_smallest, height and is_valid are
//     ITERATIVE. A recursive version uses one stack frame per level, and on a degenerate (sorted-input)
//     tree of 100,000 nodes that can overflow the stack. The traversal helpers (in_order, pre_order,
//     post_order, range) are recursive for readability, so they assume a reasonably shaped tree;
//     kth_smallest and is_valid show the explicit-stack technique that removes that limit.
//   * The destructor is iterative too: the default recursive unique_ptr destruction would also overflow
//     on a deep degenerate tree.
//   * Templated on the key type and a comparator (default std::less), like std::set.
// =====================================================================================================

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <queue>
#include <random>
#include <stack>
#include <string>
#include <vector>

template <typename Key, typename Compare = std::less<Key>>
class BinarySearchTree {
    struct Node {
        explicit Node(Key k) : key(std::move(k)) {}
        Key key;
        std::unique_ptr<Node> left, right;
    };

public:
    BinarySearchTree() = default;
    BinarySearchTree(std::initializer_list<Key> keys) {
        for (const Key& k : keys) insert(k);
    }
    ~BinarySearchTree() { clear(); }
    BinarySearchTree(BinarySearchTree&&) noexcept = default;
    BinarySearchTree& operator=(BinarySearchTree&&) noexcept = default;
    BinarySearchTree(const BinarySearchTree&) = delete;  // deep copy left out to keep the example focused
    BinarySearchTree& operator=(const BinarySearchTree&) = delete;

    // ------------------------------------------------------------------ modify

    /// Returns false if the key is already present.
    bool insert(Key key) {
        std::unique_ptr<Node>* link = &root_;  // the pointer that will hold the new node
        while (*link) {
            if (less_(key, (*link)->key)) link = &(*link)->left;
            else if (less_((*link)->key, key)) link = &(*link)->right;
            else return false;  // duplicate
        }
        *link = std::make_unique<Node>(std::move(key));
        ++size_;
        return true;
    }

    /// Returns false if the key was not present.
    bool erase(const Key& key) {
        std::unique_ptr<Node>* link = find_link(key);
        if (!*link) return false;
        erase_node(*link);
        --size_;
        return true;
    }

    void clear() noexcept {
        // Iterative teardown: detach children onto a stack so no deep recursion happens.
        std::vector<std::unique_ptr<Node>> pending;
        if (root_) pending.push_back(std::move(root_));
        while (!pending.empty()) {
            std::unique_ptr<Node> node = std::move(pending.back());
            pending.pop_back();
            if (node->left) pending.push_back(std::move(node->left));
            if (node->right) pending.push_back(std::move(node->right));
        }  // `node` is destroyed here with no children attached
        size_ = 0;
    }

    // ------------------------------------------------------------------ query

    [[nodiscard]] bool contains(const Key& key) const { return find(key) != nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::optional<Key> min() const {
        const Node* n = root_.get();
        if (!n) return std::nullopt;
        while (n->left) n = n->left.get();
        return n->key;
    }

    [[nodiscard]] std::optional<Key> max() const {
        const Node* n = root_.get();
        if (!n) return std::nullopt;
        while (n->right) n = n->right.get();
        return n->key;
    }

    /// Largest key <= key.
    [[nodiscard]] std::optional<Key> floor(const Key& key) const {
        std::optional<Key> best;
        for (const Node* n = root_.get(); n;) {
            if (less_(key, n->key)) {
                n = n->left.get();  // too big: answer must be on the left
            } else {
                best = n->key;  // n->key <= key: a candidate, but there may be a bigger one on the right
                if (!less_(n->key, key)) break;  // exact match
                n = n->right.get();
            }
        }
        return best;
    }

    /// Smallest key >= key.
    [[nodiscard]] std::optional<Key> ceiling(const Key& key) const {
        std::optional<Key> best;
        for (const Node* n = root_.get(); n;) {
            if (less_(n->key, key)) {
                n = n->right.get();
            } else {
                best = n->key;
                if (!less_(key, n->key)) break;
                n = n->left.get();
            }
        }
        return best;
    }

    /// Smallest key strictly greater than key (works even if key is not in the tree).
    [[nodiscard]] std::optional<Key> successor(const Key& key) const {
        std::optional<Key> best;
        for (const Node* n = root_.get(); n;) {
            if (less_(key, n->key)) {
                best = n->key;
                n = n->left.get();
            } else {
                n = n->right.get();
            }
        }
        return best;
    }

    /// Largest key strictly less than key.
    [[nodiscard]] std::optional<Key> predecessor(const Key& key) const {
        std::optional<Key> best;
        for (const Node* n = root_.get(); n;) {
            if (less_(n->key, key)) {
                best = n->key;
                n = n->right.get();
            } else {
                n = n->left.get();
            }
        }
        return best;
    }

    /// k-th smallest key, 1-based. Iterative in-order walk that stops early.
    [[nodiscard]] std::optional<Key> kth_smallest(std::size_t k) const {
        std::stack<const Node*> stack;
        const Node* n = root_.get();
        while (n || !stack.empty()) {
            while (n) {  // go as far left as possible, remembering the path
                stack.push(n);
                n = n->left.get();
            }
            n = stack.top();
            stack.pop();
            if (--k == 0) return n->key;
            n = n->right.get();
        }
        return std::nullopt;
    }

    /// All keys in [lo, hi], sorted. Skips subtrees that cannot contain keys in the range.
    [[nodiscard]] std::vector<Key> range(const Key& lo, const Key& hi) const {
        std::vector<Key> out;
        range(root_.get(), lo, hi, out);
        return out;
    }

    /// Number of nodes on the longest root-to-leaf path (empty tree = 0). Iterative (level by level).
    [[nodiscard]] std::size_t height() const {
        std::size_t levels = 0;
        std::queue<const Node*> q;
        if (root_) q.push(root_.get());
        while (!q.empty()) {
            ++levels;
            for (std::size_t i = q.size(); i > 0; --i) {
                const Node* n = q.front();
                q.pop();
                if (n->left) q.push(n->left.get());
                if (n->right) q.push(n->right.get());
            }
        }
        return levels;
    }

    /// Checks the BST invariant: an in-order walk must produce strictly increasing keys.
    /// Iterative (explicit stack), so it is safe even on a degenerate tree.
    [[nodiscard]] bool is_valid() const {
        std::stack<const Node*> stack;
        const Node* previous = nullptr;
        const Node* n = root_.get();
        while (n || !stack.empty()) {
            while (n) {
                stack.push(n);
                n = n->left.get();
            }
            n = stack.top();
            stack.pop();
            if (previous && !less_(previous->key, n->key)) return false;  // out of order (or a duplicate)
            previous = n;
            n = n->right.get();
        }
        return true;
    }

    // ------------------------------------------------------------------ traversals

    template <typename F>
    void in_order(F&& visit) const { in_order(root_.get(), visit); }
    template <typename F>
    void pre_order(F&& visit) const { pre_order(root_.get(), visit); }
    template <typename F>
    void post_order(F&& visit) const { post_order(root_.get(), visit); }

    /// Breadth-first: returns the keys level by level.
    [[nodiscard]] std::vector<std::vector<Key>> levels() const {
        std::vector<std::vector<Key>> out;
        std::queue<const Node*> q;
        if (root_) q.push(root_.get());
        while (!q.empty()) {
            auto& level = out.emplace_back();
            for (std::size_t i = q.size(); i > 0; --i) {
                const Node* n = q.front();
                q.pop();
                level.push_back(n->key);
                if (n->left) q.push(n->left.get());
                if (n->right) q.push(n->right.get());
            }
        }
        return out;
    }

    /// Draws the tree sideways (root on the left, right subtree on top).
    void print(std::string_view indent = "   ") const {
        if (!root_) return std::println("{}(empty)", indent);
        print(root_.get(), std::string(indent), true, true);
    }

private:
    const Node* find(const Key& key) const {
        const Node* n = root_.get();
        while (n) {
            if (less_(key, n->key)) n = n->left.get();
            else if (less_(n->key, key)) n = n->right.get();
            else return n;
        }
        return nullptr;
    }

    // Returns the owning pointer (root_ or a parent's left/right) that holds `key`, or an empty one.
    std::unique_ptr<Node>* find_link(const Key& key) {
        std::unique_ptr<Node>* link = &root_;
        while (*link) {
            if (less_(key, (*link)->key)) link = &(*link)->left;
            else if (less_((*link)->key, key)) link = &(*link)->right;
            else break;
        }
        return link;
    }

    // Removes the node held by `link`, keeping the BST invariant.
    void erase_node(std::unique_ptr<Node>& link) {
        Node* n = link.get();
        if (!n->left) {         // case 1 (leaf) and case 2 (only a right child)
            link = std::move(n->right);  // the old node is destroyed by this assignment
        } else if (!n->right) { // case 2 (only a left child)
            link = std::move(n->left);
        } else {                // case 3: two children
            // Find the in-order successor: the leftmost node of the right subtree.
            std::unique_ptr<Node>* succ = &n->right;
            while ((*succ)->left) succ = &(*succ)->left;
            n->key = std::move((*succ)->key);   // the successor's key takes this node's place
            *succ = std::move((*succ)->right);  // the successor has no left child: splice in its right
        }
    }

    void range(const Node* n, const Key& lo, const Key& hi, std::vector<Key>& out) const {
        if (!n) return;
        if (less_(lo, n->key)) range(n->left.get(), lo, hi, out);                // left side may have keys >= lo
        if (!less_(n->key, lo) && !less_(hi, n->key)) out.push_back(n->key);
        if (less_(n->key, hi)) range(n->right.get(), lo, hi, out);               // right side may have keys <= hi
    }

    template <typename F>
    static void in_order(const Node* n, F& visit) {
        if (!n) return;
        in_order(n->left.get(), visit);
        visit(n->key);
        in_order(n->right.get(), visit);
    }
    template <typename F>
    static void pre_order(const Node* n, F& visit) {
        if (!n) return;
        visit(n->key);
        pre_order(n->left.get(), visit);
        pre_order(n->right.get(), visit);
    }
    template <typename F>
    static void post_order(const Node* n, F& visit) {
        if (!n) return;
        post_order(n->left.get(), visit);
        post_order(n->right.get(), visit);
        visit(n->key);
    }

    void print(const Node* n, const std::string& prefix, bool is_right, bool is_root) const {
        if (n->right) print(n->right.get(), prefix + (is_root ? "" : (is_right ? "        " : "│       ")), true, false);
        std::println("{}{}{}", prefix, is_root ? "" : (is_right ? "┌────── " : "└────── "), n->key);
        if (n->left) print(n->left.get(), prefix + (is_root ? "" : (is_right ? "│       " : "        ")), false, false);
    }

    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
    [[no_unique_address]] Compare less_;
};

// ===================================================================== demo helpers

template <typename T>
std::string join(const std::vector<T>& v) {
    std::string out;
    for (const T& x : v) out += (out.empty() ? "" : " ") + std::format("{}", x);
    return out;
}

template <typename T>
std::string show(const std::optional<T>& v) { return v ? std::format("{}", *v) : std::string("none"); }

// ===================================================================== demo

int main() {
    BinarySearchTree<int> bst;

    std::println("1) Insert 50 30 70 20 40 60 80 35 45 65");
    for (int k : {50, 30, 70, 20, 40, 60, 80, 35, 45, 65}) bst.insert(k);
    bst.print();
    std::println("   size {}   height {}   valid BST: {}", bst.size(), bst.height(), bst.is_valid());
    std::println("   insert 40 again -> {} (duplicates are rejected)", bst.insert(40));

    std::println("\n2) Traversals");
    std::vector<int> in, pre, post;
    bst.in_order([&](int k) { in.push_back(k); });
    bst.pre_order([&](int k) { pre.push_back(k); });
    bst.post_order([&](int k) { post.push_back(k); });
    std::println("   in-order    {}   <- always sorted", join(in));
    std::println("   pre-order   {}   <- root first (re-inserting in this order rebuilds the same tree)", join(pre));
    std::println("   post-order  {}   <- children before parent (safe order to delete)", join(post));
    std::print("   level-order");
    for (const auto& level : bst.levels()) std::print("  [{}]", join(level));
    std::println("");

    std::println("\n3) Searching");
    for (int k : {45, 55}) std::println("   contains({}) = {}", k, bst.contains(k));
    std::println("   min = {}   max = {}", show(bst.min()), show(bst.max()));
    std::println("   floor(55) = {}   ceiling(55) = {}   (55 is not in the tree)", show(bst.floor(55)), show(bst.ceiling(55)));
    std::println("   floor(40) = {}   ceiling(40) = {}   (exact match)", show(bst.floor(40)), show(bst.ceiling(40)));
    std::println("   predecessor(50) = {}   successor(50) = {}", show(bst.predecessor(50)), show(bst.successor(50)));
    std::println("   floor(10) = {}   ceiling(90) = {}   (nothing that small / that large)", show(bst.floor(10)), show(bst.ceiling(90)));
    std::println("   3rd smallest = {}   8th smallest = {}", show(bst.kth_smallest(3)), show(bst.kth_smallest(8)));
    std::println("   keys in [33, 62] = {}", join(bst.range(33, 62)));

    std::println("\n4) Deleting: the three cases");
    std::println("   erase(30): TWO children (20 and 40) -> 30 is replaced by its in-order successor 35,");
    std::println("              the smallest key in its right subtree; that node is then removed from the bottom");
    bst.erase(30);
    bst.print();
    std::println("   erase(20): a LEAF -> simply removed");
    bst.erase(20);
    std::println("   erase(60): ONE child (65) -> 65 moves up into 60's place");
    bst.erase(60);
    bst.print();
    std::println("   erase(50): the ROOT, two children -> its successor 65 becomes the new root");
    bst.erase(50);
    bst.print();
    std::println("   erase(99) -> {} (not present)", bst.erase(99));
    std::println("   size {}   height {}   valid BST: {}", bst.size(), bst.height(), bst.is_valid());

    std::println("\n5) Why balance matters: 100,000 keys inserted in random vs sorted order");
    std::vector<int> keys(100'000);
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) keys[static_cast<std::size_t>(i)] = i;
    BinarySearchTree<int> sorted_tree;
    for (int k : keys) sorted_tree.insert(k);  // each key becomes the right child of the previous one
    std::ranges::shuffle(keys, std::mt19937{42});
    BinarySearchTree<int> random_tree;
    for (int k : keys) random_tree.insert(k);
    std::println("   random order: height {:>6}   (log2(100,000) is about 17; random BSTs average about 2.99·log2 n)", random_tree.height());
    std::println("   sorted order: height {:>6}   (a linked list: every search walks up to 100,000 nodes)", sorted_tree.height());
    std::println("   both are valid BSTs: {} / {}   (the iterative code also avoided a stack overflow on the deep one)",
                 random_tree.is_valid(), sorted_tree.is_valid());

    std::println("\n6) Works for any ordered type: strings in reverse order with std::greater");
    BinarySearchTree<std::string, std::greater<>> words{"pear", "apple", "mango", "kiwi", "banana"};
    std::vector<std::string> w;
    words.in_order([&](const std::string& s) { w.push_back(s); });
    std::println("   in-order: {}", join(w));
}
