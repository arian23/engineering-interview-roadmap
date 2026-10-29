// =====================================================================================================
// main.cpp: Binary Search Tree DEMO
// =====================================================================================================
// Uses only the interface in bst.hpp. The implementation is in bst.cpp, compiled separately.
// Theory and complexity are documented at the top of bst.hpp; implementation notes at the top of bst.cpp.
// =====================================================================================================

#include "bst.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <vector>

namespace {

template <typename T>
std::string join(const std::vector<T>& v) {
    std::string out;
    for (const T& x : v) out += (out.empty() ? "" : " ") + std::format("{}", x);
    return out;
}

template <typename T>
std::string show(const std::optional<T>& v) {
    return v ? std::format("{}", *v) : std::string("none");
}

}  // namespace

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
    for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = static_cast<int>(i);
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
