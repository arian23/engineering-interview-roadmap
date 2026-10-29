#pragma once
// =====================================================================================================
// bst.hpp: Binary Search Tree, INTERFACE (C++23)
// =====================================================================================================
//
// This header says WHAT the tree can do. HOW it does it lives in bst.cpp, which is compiled separately.
//
// WHAT A BST IS
//   A binary tree (each node has at most two children) that keeps one rule, the BST INVARIANT:
//
//       every key in a node's LEFT subtree  <  the node's key  <  every key in its RIGHT subtree
//
//   The rule holds at every node. A search can therefore discard one whole subtree at each step, like
//   binary search on a sorted array, while still allowing cheap inserts and deletes.
//
//               50
//             /    \          a valid BST
//           30      70          in-order traversal visits keys in SORTED order:
//          /  \    /  \         20 30 40 50 60 70 80
//        20   40  60   80
//
// COMPLEXITY
//   Every search, insert or delete walks one root-to-leaf path: O(h), where h is the height.
//   * Random insertion order: h is about 3·log2(n) on average, so O(log n).
//   * Sorted insertion order: every key becomes the right child of the previous one, h = n, so O(n).
//   Self-balancing trees (AVL, red-black) fix this; std::set and std::map are red-black trees.
//
// HOW THE CODE IS SPLIT
//   bst.hpp  (this file)  the class declaration and documentation. The node type is only DECLARED
//                         here (struct Node;), so users cannot depend on the tree's internals.
//   bst.cpp               every function body, plus the list of key types the tree is compiled for.
//   main.cpp              a demo that uses only this header.
//
//   A template is normally defined entirely in its header, because the compiler needs the full code
//   for every key type it is used with. Here the definitions stay in bst.cpp, and bst.cpp EXPLICITLY
//   INSTANTIATES the tree for a fixed set of types (see the end of this file). Using any other key type
//   is a link error until you add one line to bst.cpp:
//       template class BinarySearchTree<YourKey>;
//   Benefits: the implementation is compiled once, files that include this header compile faster,
//   and changing an implementation detail does not change the header.
// =====================================================================================================

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

template <typename Key, typename Compare = std::less<Key>>
class BinarySearchTree {
public:
    /// Called once per key by the traversal functions.
    using Visitor = std::function<void(const Key&)>;

    // ------------------------------------------------------------------ lifetime

    BinarySearchTree();
    BinarySearchTree(std::initializer_list<Key> keys);  ///< inserts each key in order
    ~BinarySearchTree();                                ///< iterative: safe even for a very deep tree

    BinarySearchTree(BinarySearchTree&& other) noexcept;  ///< `other` is left empty
    BinarySearchTree& operator=(BinarySearchTree&& other) noexcept;
    BinarySearchTree(const BinarySearchTree&) = delete;  ///< deep copy left out to keep the example focused
    BinarySearchTree& operator=(const BinarySearchTree&) = delete;

    // ------------------------------------------------------------------ modify (O(h))

    /// Adds `key`. Returns false (and changes nothing) if it is already present: keys are unique.
    bool insert(Key key);

    /// Removes `key`. Returns false if it was not present.
    /// Handles the three cases: a leaf, a node with one child, and a node with two children
    /// (replaced by its in-order successor).
    bool erase(const Key& key);

    /// Removes every key. O(n).
    void clear() noexcept;

    // ------------------------------------------------------------------ query (O(h) unless noted)

    [[nodiscard]] bool contains(const Key& key) const;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }  ///< O(1)
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }   ///< O(1)

    [[nodiscard]] std::optional<Key> min() const;                       ///< smallest key
    [[nodiscard]] std::optional<Key> max() const;                       ///< largest key
    [[nodiscard]] std::optional<Key> floor(const Key& key) const;       ///< largest key <= key
    [[nodiscard]] std::optional<Key> ceiling(const Key& key) const;     ///< smallest key >= key
    [[nodiscard]] std::optional<Key> predecessor(const Key& key) const; ///< largest key < key
    [[nodiscard]] std::optional<Key> successor(const Key& key) const;   ///< smallest key > key
    [[nodiscard]] std::optional<Key> kth_smallest(std::size_t k) const; ///< 1-based. O(h + k)

    /// All keys in [lo, hi], sorted. Skips subtrees that cannot overlap the range. O(h + number found).
    [[nodiscard]] std::vector<Key> range(const Key& lo, const Key& hi) const;

    /// Number of levels (empty tree = 0, single node = 1). O(n).
    [[nodiscard]] std::size_t height() const;

    /// True if the BST invariant holds at every node. O(n).
    [[nodiscard]] bool is_valid() const;

    // ------------------------------------------------------------------ traversals (O(n))

    void in_order(const Visitor& visit) const;    ///< left, node, right: keys in sorted order
    void pre_order(const Visitor& visit) const;   ///< node, left, right: re-inserting in this order rebuilds the tree
    void post_order(const Visitor& visit) const;  ///< left, right, node: children before parent
    [[nodiscard]] std::vector<std::vector<Key>> levels() const;  ///< breadth-first, one vector per level

    /// Draws the tree sideways: the root on the left, the right subtree above it.
    void print(std::string_view indent = "   ") const;

private:
    struct Node;  // defined in bst.cpp: an implementation detail

    const Node* find(const Key& key) const;
    std::unique_ptr<Node>* find_link(const Key& key);
    static void erase_node(std::unique_ptr<Node>& link);
    void collect_range(const Node* n, const Key& lo, const Key& hi, std::vector<Key>& out) const;
    static void in_order(const Node* n, const Visitor& visit);
    static void pre_order(const Node* n, const Visitor& visit);
    static void post_order(const Node* n, const Visitor& visit);
    static void print(const Node* n, const std::string& prefix, bool is_right, bool is_root);

    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
    [[no_unique_address]] Compare less_;
};

// The key types bst.cpp compiles the tree for. `extern template` tells every file that includes this
// header: "don't try to build these yourself, the code is already compiled in bst.cpp".
extern template class BinarySearchTree<int>;
extern template class BinarySearchTree<std::string, std::greater<>>;
