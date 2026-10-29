// =====================================================================================================
// bst.cpp: Binary Search Tree, IMPLEMENTATION (C++23)
// =====================================================================================================
//
// Every member function declared in bst.hpp is defined here. At the bottom, the tree is explicitly
// instantiated for the key types listed in bst.hpp.
//
// IMPLEMENTATION CHOICES
//   * Nodes are owned by std::unique_ptr: the tree owns the root, each node owns its children. There is
//     no manual `delete`, so erase() and clear() cannot leak (RAII).
//   * Pointer-to-link technique: insert(), find_link() and erase_node() work with
//     std::unique_ptr<Node>* "links" (the root pointer, or a parent's left/right pointer). Rewiring a link
//     works the same way whether the node is the root or deep in the tree, so there is no special case
//     for the root and no parent pointers are needed.
//   * Iterative where depth matters: insert, contains, erase, floor/ceiling, predecessor/successor,
//     kth_smallest, height, is_valid and the destructor are loops. Recursion uses one stack frame per
//     level, and a degenerate tree built from sorted input can be 100,000 levels deep, which would
//     overflow the stack. The traversal and range helpers stay recursive for readability, so they
//     assume a reasonably shaped tree.
// =====================================================================================================

#include "bst.hpp"

#include <format>
#include <print>
#include <queue>
#include <stack>
#include <utility>

template <typename Key, typename Compare>
struct BinarySearchTree<Key, Compare>::Node {
    explicit Node(Key k) : key(std::move(k)) {}
    Key key;
    std::unique_ptr<Node> left, right;
};

// ===================================================================== lifetime

template <typename Key, typename Compare>
BinarySearchTree<Key, Compare>::BinarySearchTree() = default;

template <typename Key, typename Compare>
BinarySearchTree<Key, Compare>::BinarySearchTree(std::initializer_list<Key> keys) {
    for (const Key& k : keys) insert(k);
}

template <typename Key, typename Compare>
BinarySearchTree<Key, Compare>::~BinarySearchTree() {
    clear();
}

template <typename Key, typename Compare>
BinarySearchTree<Key, Compare>::BinarySearchTree(BinarySearchTree&& other) noexcept
    : root_(std::move(other.root_)), size_(std::exchange(other.size_, 0)), less_(std::move(other.less_)) {}

template <typename Key, typename Compare>
BinarySearchTree<Key, Compare>& BinarySearchTree<Key, Compare>::operator=(BinarySearchTree&& other) noexcept {
    if (this != &other) {
        clear();
        root_ = std::move(other.root_);
        size_ = std::exchange(other.size_, 0);
        less_ = std::move(other.less_);
    }
    return *this;
}

// ===================================================================== modify

template <typename Key, typename Compare>
bool BinarySearchTree<Key, Compare>::insert(Key key) {
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

template <typename Key, typename Compare>
bool BinarySearchTree<Key, Compare>::erase(const Key& key) {
    std::unique_ptr<Node>* link = find_link(key);
    if (!*link) return false;
    erase_node(*link);
    --size_;
    return true;
}

// Removes the node held by `link` while keeping the BST invariant.
template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::erase_node(std::unique_ptr<Node>& link) {
    Node* n = link.get();
    if (!n->left) {          // case 1 (leaf) and case 2 (only a right child)
        link = std::move(n->right);  // the old node is destroyed by this assignment
    } else if (!n->right) {  // case 2 (only a left child)
        link = std::move(n->left);
    } else {                 // case 3: two children
        // The in-order successor is the leftmost node of the right subtree. It has no left child, so
        // removing it is case 1 or 2.
        std::unique_ptr<Node>* succ = &n->right;
        while ((*succ)->left) succ = &(*succ)->left;
        n->key = std::move((*succ)->key);   // the successor's key takes this node's place
        *succ = std::move((*succ)->right);  // splice out the successor
    }
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::clear() noexcept {
    // Detach children onto an explicit stack, so destroying a deep tree never recurses.
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

// ===================================================================== lookup helpers

template <typename Key, typename Compare>
auto BinarySearchTree<Key, Compare>::find(const Key& key) const -> const Node* {
    const Node* n = root_.get();
    while (n) {
        if (less_(key, n->key)) n = n->left.get();
        else if (less_(n->key, key)) n = n->right.get();
        else return n;
    }
    return nullptr;
}

// Returns the owning pointer (root_ or a parent's left/right) that holds `key`, or an empty one.
template <typename Key, typename Compare>
auto BinarySearchTree<Key, Compare>::find_link(const Key& key) -> std::unique_ptr<Node>* {
    std::unique_ptr<Node>* link = &root_;
    while (*link) {
        if (less_(key, (*link)->key)) link = &(*link)->left;
        else if (less_((*link)->key, key)) link = &(*link)->right;
        else break;
    }
    return link;
}

// ===================================================================== queries

template <typename Key, typename Compare>
bool BinarySearchTree<Key, Compare>::contains(const Key& key) const {
    return find(key) != nullptr;
}

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::min() const {
    const Node* n = root_.get();
    if (!n) return std::nullopt;
    while (n->left) n = n->left.get();
    return n->key;
}

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::max() const {
    const Node* n = root_.get();
    if (!n) return std::nullopt;
    while (n->right) n = n->right.get();
    return n->key;
}

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::floor(const Key& key) const {
    std::optional<Key> best;
    for (const Node* n = root_.get(); n;) {
        if (less_(key, n->key)) {
            n = n->left.get();  // too big: the answer is on the left
        } else {
            best = n->key;  // n->key <= key: a candidate, but there may be a bigger one on the right
            if (!less_(n->key, key)) break;  // exact match
            n = n->right.get();
        }
    }
    return best;
}

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::ceiling(const Key& key) const {
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

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::successor(const Key& key) const {
    std::optional<Key> best;
    for (const Node* n = root_.get(); n;) {
        if (less_(key, n->key)) {
            best = n->key;  // bigger than key: a candidate; look for a smaller one on the left
            n = n->left.get();
        } else {
            n = n->right.get();
        }
    }
    return best;
}

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::predecessor(const Key& key) const {
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

template <typename Key, typename Compare>
std::optional<Key> BinarySearchTree<Key, Compare>::kth_smallest(std::size_t k) const {
    // Iterative in-order walk with an explicit stack, stopping after the k-th key.
    std::stack<const Node*> stack;
    const Node* n = root_.get();
    while (n || !stack.empty()) {
        while (n) {  // go as far left as possible, remembering the path back up
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

template <typename Key, typename Compare>
std::vector<Key> BinarySearchTree<Key, Compare>::range(const Key& lo, const Key& hi) const {
    std::vector<Key> out;
    collect_range(root_.get(), lo, hi, out);
    return out;
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::collect_range(const Node* n, const Key& lo, const Key& hi, std::vector<Key>& out) const {
    if (!n) return;
    if (less_(lo, n->key)) collect_range(n->left.get(), lo, hi, out);  // the left side may hold keys >= lo
    if (!less_(n->key, lo) && !less_(hi, n->key)) out.push_back(n->key);
    if (less_(n->key, hi)) collect_range(n->right.get(), lo, hi, out);  // the right side may hold keys <= hi
}

template <typename Key, typename Compare>
std::size_t BinarySearchTree<Key, Compare>::height() const {
    std::size_t levels = 0;  // counted level by level (breadth-first), so no recursion
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

template <typename Key, typename Compare>
bool BinarySearchTree<Key, Compare>::is_valid() const {
    // In a valid BST the in-order sequence is strictly increasing. Iterative, so a deep tree is fine.
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
        if (previous && !less_(previous->key, n->key)) return false;  // out of order, or a duplicate
        previous = n;
        n = n->right.get();
    }
    return true;
}

// ===================================================================== traversals

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::in_order(const Visitor& visit) const {
    in_order(root_.get(), visit);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::pre_order(const Visitor& visit) const {
    pre_order(root_.get(), visit);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::post_order(const Visitor& visit) const {
    post_order(root_.get(), visit);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::in_order(const Node* n, const Visitor& visit) {
    if (!n) return;
    in_order(n->left.get(), visit);
    visit(n->key);
    in_order(n->right.get(), visit);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::pre_order(const Node* n, const Visitor& visit) {
    if (!n) return;
    visit(n->key);
    pre_order(n->left.get(), visit);
    pre_order(n->right.get(), visit);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::post_order(const Node* n, const Visitor& visit) {
    if (!n) return;
    post_order(n->left.get(), visit);
    post_order(n->right.get(), visit);
    visit(n->key);
}

template <typename Key, typename Compare>
std::vector<std::vector<Key>> BinarySearchTree<Key, Compare>::levels() const {
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

// ===================================================================== printing

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::print(std::string_view indent) const {
    if (!root_) {
        std::println("{}(empty)", indent);
        return;
    }
    print(root_.get(), std::string(indent), true, true);
}

template <typename Key, typename Compare>
void BinarySearchTree<Key, Compare>::print(const Node* n, const std::string& prefix, bool is_right, bool is_root) {
    // Right subtree first (printed above), then this node, then the left subtree (below).
    if (n->right) print(n->right.get(), prefix + (is_root ? "" : (is_right ? "        " : "│       ")), true, false);
    std::println("{}{}{}", prefix, is_root ? "" : (is_right ? "┌────── " : "└────── "), n->key);
    if (n->left) print(n->left.get(), prefix + (is_root ? "" : (is_right ? "│       " : "        ")), false, false);
}

// ===================================================================== explicit instantiations
// Compile the whole class for these key types. Each one must also be listed as `extern template`
// in bst.hpp. To support another key type, add one line in both places.

template class BinarySearchTree<int>;
template class BinarySearchTree<std::string, std::greater<>>;
