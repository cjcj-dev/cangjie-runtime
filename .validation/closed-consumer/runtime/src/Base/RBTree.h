// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Intrusive red-black tree, the subset of HotSpot utilities/rbTree.hpp:60-330
// (IntrusiveRBNode, AbstractRBTree with Cursor, insert_at_cursor,
// replace_at_cursor, remove, next/prev, leftmost/rightmost, verify_self) that
// ZMappedCache consumes (zMappedCache.hpp:51-84). The node-owning RBTree<K,V>
// with an allocator is not needed here.

#ifndef MRT_BASE_RB_TREE_H
#define MRT_BASE_RB_TREE_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace MapleRuntime {

enum class RBTreeOrdering : int { LT, EQ, GT };

template <typename K, typename NodeType, typename COMPARATOR>
class AbstractRBTree;

class IntrusiveRBNode {
  template <typename K, typename NodeType, typename COMPARATOR>
  friend class AbstractRBTree;

  uintptr_t _parent; // LSB encodes color information. 0 = RED, 1 = BLACK
  IntrusiveRBNode* _left;
  IntrusiveRBNode* _right;

public:
  IntrusiveRBNode() : _parent(0), _left(nullptr), _right(nullptr) {}

  // Gets the previous in-order node in the tree.
  // nullptr is returned if there is no previous node.
  const IntrusiveRBNode* prev() const {
    const IntrusiveRBNode* node = this;
    if (_left != nullptr) { // left subtree exists
      node = _left;
      while (node->_right != nullptr) {
        node = node->_right;
      }
      return node;
    }

    while (node != nullptr && node->is_left_child()) {
      node = node->parent();
    }
    return node->parent();
  }

  IntrusiveRBNode* prev() {
    return const_cast<IntrusiveRBNode*>(static_cast<const IntrusiveRBNode*>(this)->prev());
  }

  // Gets the next in-order node in the tree.
  // nullptr is returned if there is no next node.
  const IntrusiveRBNode* next() const {
    const IntrusiveRBNode* node = this;
    if (_right != nullptr) { // right subtree exists
      node = _right;
      while (node->_left != nullptr) {
        node = node->_left;
      }
      return node;
    }

    while (node != nullptr && node->is_right_child()) {
      node = node->parent();
    }
    return node->parent();
  }

  IntrusiveRBNode* next() {
    return const_cast<IntrusiveRBNode*>(static_cast<const IntrusiveRBNode*>(this)->next());
  }

private:
  bool is_black() const { return (_parent & 0x1) != 0; }
  bool is_red() const { return (_parent & 0x1) == 0; }

  void set_black() { _parent |= 0x1; }
  void set_red() { _parent &= ~static_cast<uintptr_t>(0x1); }

  IntrusiveRBNode* parent() const { return reinterpret_cast<IntrusiveRBNode*>(_parent & ~static_cast<uintptr_t>(0x1)); }
  void set_parent(IntrusiveRBNode* new_parent) { _parent = (_parent & 0x1) | reinterpret_cast<uintptr_t>(new_parent); }

  bool is_right_child() const { return parent() != nullptr && parent()->_right == this; }
  bool is_left_child() const { return parent() != nullptr && parent()->_left == this; }

  void replace_child(IntrusiveRBNode* old_child, IntrusiveRBNode* new_child) {
    if (_left == old_child) {
      _left = new_child;
    } else if (_right == old_child) {
      _right = new_child;
    } else {
      assert(false && "replace_child: not a child");
    }
  }

  // This node down, right child up
  // Returns right child (now parent)
  IntrusiveRBNode* rotate_left() {
    IntrusiveRBNode* old_right = _right;

    _right = old_right->_left;
    if (_right != nullptr) {
      _right->set_parent(this);
    }

    old_right->set_parent(parent());
    if (parent() != nullptr) {
      parent()->replace_child(this, old_right);
    }

    old_right->_left = this;
    set_parent(old_right);

    return old_right;
  }

  // This node down, left child up
  // Returns left child (now parent)
  IntrusiveRBNode* rotate_right() {
    IntrusiveRBNode* old_left = _left;

    _left = old_left->_right;
    if (_left != nullptr) {
      _left->set_parent(this);
    }

    old_left->set_parent(parent());
    if (parent() != nullptr) {
      parent()->replace_child(this, old_left);
    }

    old_left->_right = this;
    set_parent(old_left);

    return old_left;
  }
};

// COMPARATOR provides
//   static RBTreeOrdering cmp(const K& key, const NodeType* node);   (search)
//   static bool less_than(const NodeType* a, const NodeType* b);     (verification)
template <typename K, typename NodeType, typename COMPARATOR>
class AbstractRBTree {
public:
  // Represents the location of a (would be) node in the tree.
  // If a cursor is valid (valid() == true) it points somewhere in the tree.
  // If the cursor points to an existing node (found() == true), node() can be used to access that node.
  // If no node is pointed to, node() returns null, regardless if the cursor is valid or not.
  class Cursor {
    friend AbstractRBTree<K, NodeType, COMPARATOR>;
    NodeType** _insert_location;
    NodeType* _parent;
    Cursor() : _insert_location(nullptr), _parent(nullptr) {}
    Cursor(NodeType** insert_location, NodeType* parent) : _insert_location(insert_location), _parent(parent) {}

  public:
    bool valid() const { return _insert_location != nullptr; }
    bool found() const { return *_insert_location != nullptr; }
    NodeType* node() const { return _insert_location == nullptr ? nullptr : *_insert_location; }
  };

protected:
  size_t _num_nodes;
  IntrusiveRBNode* _root;

private:
  AbstractRBTree(const AbstractRBTree&) = delete;
  AbstractRBTree& operator=(const AbstractRBTree&) = delete;

  static RBTreeOrdering cmp(const K& a, const NodeType* b) { return COMPARATOR::cmp(a, b); }
  static bool less_than(const NodeType* a, const NodeType* b) { return COMPARATOR::less_than(a, b); }

  // True if node is black (nil nodes count as black)
  static bool is_black(const IntrusiveRBNode* node) { return node == nullptr || node->is_black(); }
  static bool is_red(const IntrusiveRBNode* node) { return node != nullptr && node->is_red(); }

  void fix_insert_violations(IntrusiveRBNode* node) {
    if (node->is_black()) { // node's value was updated
      return;               // Tree is already correct
    }

    IntrusiveRBNode* parent = node->parent();
    while (parent != nullptr && parent->is_red()) {
      // Node and parent are both red, creating a red-violation

      IntrusiveRBNode* grandparent = parent->parent();
      if (grandparent == nullptr) { // Parent is the tree root
        assert(parent == _root);
        parent->set_black(); // Color parent black to eliminate the red-violation
        return;
      }

      IntrusiveRBNode* uncle = parent->is_left_child() ? grandparent->_right : grandparent->_left;
      if (is_black(uncle)) { // Parent is red, uncle is black
        // Rotate the parent to the position of the grandparent
        if (parent->is_left_child()) {
          if (node->is_right_child()) { // Node is an "inner" node
            // Rotate and swap node and parent to make it an "outer" node
            parent->rotate_left();
            parent = node;
          }
          grandparent->rotate_right(); // Rotate the parent to the position of the grandparent
        } else if (parent->is_right_child()) {
          if (node->is_left_child()) { // Node is an "inner" node
            // Rotate and swap node and parent to make it an "outer" node
            parent->rotate_right();
            parent = node;
          }
          grandparent->rotate_left(); // Rotate the parent to the position of the grandparent
        }

        // Swap parent and grandparent colors to eliminate the red-violation
        parent->set_black();
        grandparent->set_red();

        if (_root == grandparent) {
          _root = parent;
        }

        return;
      }

      // Parent and uncle are both red
      // Paint both black, paint grandparent red to not create a black-violation
      parent->set_black();
      uncle->set_black();
      grandparent->set_red();

      // Move up two levels to check for new potential red-violation
      node = grandparent;
      parent = grandparent->parent();
    }
  }

  void remove_black_leaf(IntrusiveRBNode* node) {
    // Black node removed, balancing needed
    IntrusiveRBNode* parent = node->parent();
    while (parent != nullptr) {
      // Sibling must exist. If it did not, node would need to be red to not break
      // tree properties, and could be trivially removed before reaching here
      IntrusiveRBNode* sibling = node->is_left_child() ? parent->_right : parent->_left;
      if (is_red(sibling)) { // Sibling red, parent and nephews must be black
        assert(is_black(parent));
        assert(is_black(sibling->_left));
        assert(is_black(sibling->_right));
        // Swap parent and sibling colors
        parent->set_red();
        sibling->set_black();

        // Rotate parent down and sibling up
        if (node->is_left_child()) {
          parent->rotate_left();
          sibling = parent->_right;
        } else {
          parent->rotate_right();
          sibling = parent->_left;
        }

        if (_root == parent) {
          _root = parent->parent();
        }
        // Further balancing needed
      }

      IntrusiveRBNode* close_nephew = node->is_left_child() ? sibling->_left : sibling->_right;
      IntrusiveRBNode* distant_nephew = node->is_left_child() ? sibling->_right : sibling->_left;
      if (is_red(distant_nephew) || is_red(close_nephew)) {
        if (is_black(distant_nephew)) { // close red, distant black
          // Rotate sibling down and inner nephew up
          if (node->is_left_child()) {
            sibling->rotate_right();
          } else {
            sibling->rotate_left();
          }

          distant_nephew = sibling;
          sibling = close_nephew;

          distant_nephew->set_red();
          sibling->set_black();
        }

        // Distant nephew red
        // Rotate parent down and sibling up
        if (node->is_left_child()) {
          parent->rotate_left();
        } else {
          parent->rotate_right();
        }
        if (_root == parent) {
          _root = sibling;
        }

        // Swap parent and sibling colors
        if (parent->is_black()) {
          sibling->set_black();
        } else {
          sibling->set_red();
        }
        parent->set_black();

        // Color distant nephew black to restore black balance
        distant_nephew->set_black();
        return;
      }

      if (is_red(parent)) { // parent red, sibling and nephews black
        // Swap parent and sibling colors to restore black balance
        sibling->set_red();
        parent->set_black();
        return;
      }

      // Parent, sibling, and both nephews black
      // Color sibling red and move up one level
      sibling->set_red();
      node = parent;
      parent = node->parent();
    }
  }

  // Assumption: node has at most one child. Two children is handled in `remove_at_cursor()`
  void remove_from_tree(IntrusiveRBNode* node) {
    IntrusiveRBNode* parent = node->parent();
    IntrusiveRBNode* left = node->_left;
    IntrusiveRBNode* right = node->_right;
    if (left != nullptr) { // node has a left only-child
      // node must be black, and child red, otherwise a black-violation would
      // exist Remove node and color the child black.
      assert(right == nullptr);
      assert(is_black(node));
      assert(is_red(left));
      left->set_black();
      left->set_parent(parent);
      if (parent == nullptr) {
        assert(node == _root);
        _root = left;
      } else {
        parent->replace_child(node, left);
      }
    } else if (right != nullptr) { // node has a right only-child
      // node must be black, and child red, otherwise a black-violation would
      // exist Remove node and color the child black.
      assert(left == nullptr);
      assert(is_black(node));
      assert(is_red(right));
      right->set_black();
      right->set_parent(parent);
      if (parent == nullptr) {
        assert(node == _root);
        _root = right;
      } else {
        parent->replace_child(node, right);
      }
    } else {               // node has no children
      if (node == _root) { // Tree empty
        _root = nullptr;
      } else {
        if (is_black(node)) {
          // Removed node is black, creating a black imbalance
          remove_black_leaf(node);
        }
        parent->replace_child(node, nullptr);
      }
    }
  }

  size_t verify_node(const IntrusiveRBNode* node, size_t& black_height) const {
    if (node == nullptr) {
      black_height = 1;
      return 0;
    }
    assert(node->_left == nullptr || node->_left->parent() == node);
    assert(node->_right == nullptr || node->_right->parent() == node);
    if (is_red(node)) {
      assert(is_black(node->_left));
      assert(is_black(node->_right));
    }
    const NodeType* self = static_cast<const NodeType*>(node);
    if (node->_left != nullptr) {
      assert(less_than(static_cast<const NodeType*>(node->_left), self));
    }
    if (node->_right != nullptr) {
      assert(less_than(self, static_cast<const NodeType*>(node->_right)));
    }
    size_t left_black = 0;
    size_t right_black = 0;
    const size_t count = 1 + verify_node(node->_left, left_black) + verify_node(node->_right, right_black);
    assert(left_black == right_black);
    black_height = left_black + (is_black(node) ? 1 : 0);
    return count;
  }

public:
  AbstractRBTree() : _num_nodes(0), _root(nullptr) {
    static_assert(std::is_trivially_destructible<K>::value, "key type must be trivially destructable");
  }

  size_t size() const { return _num_nodes; }

  // Gets the cursor associated with the given node or key.
  Cursor cursor(const K& key) const {
    IntrusiveRBNode* parent = nullptr;
    IntrusiveRBNode* const* insert_location = &_root;

    while (*insert_location != nullptr) {
      NodeType* curr = (NodeType*)*insert_location;
      const RBTreeOrdering key_cmp_k = cmp(key, curr);

      if (key_cmp_k == RBTreeOrdering::EQ) {
        break;
      }

      parent = *insert_location;
      if (key_cmp_k == RBTreeOrdering::LT) {
        insert_location = &curr->_left;
      } else {
        insert_location = &curr->_right;
      }
    }

    return Cursor((NodeType**)insert_location, (NodeType*)parent);
  }

  Cursor cursor(const NodeType* node) const {
    if (node == nullptr) {
      return Cursor();
    }

    if (node->parent() == nullptr) {
      return Cursor((NodeType**)&_root, nullptr);
    }

    IntrusiveRBNode* parent = node->parent();
    IntrusiveRBNode** insert_location =
        node->is_left_child() ? &parent->_left : &parent->_right;
    return Cursor((NodeType**)insert_location, (NodeType*)parent);
  }

  // Moves to the next existing node.
  // If no next node exist, the cursor becomes invalid.
  Cursor next(const Cursor& node_cursor) const {
    if (node_cursor.found()) {
      return cursor(static_cast<NodeType*>(node_cursor.node()->next()));
    }

    if (node_cursor._parent == nullptr) { // Tree is empty
      return Cursor();
    }

    // Pointing to non-existant node
    if ((NodeType**)&node_cursor._parent->_left == node_cursor._insert_location) { // Left child, parent is next
      return cursor(node_cursor._parent);
    }

    return cursor(static_cast<NodeType*>(node_cursor._parent->next())); // Right child, parent's next is also node's next
  }

  // Initializes and inserts a node at the cursor location.
  // The cursor must not point to an existing node.
  void insert_at_cursor(NodeType* node, const Cursor& node_cursor) {
    assert(node != nullptr);
    assert(node_cursor.valid());
    assert(!node_cursor.found());
    _num_nodes++;

    *node_cursor._insert_location = node;

    node->set_parent(node_cursor._parent);
    node->set_red();
    node->_left = nullptr;
    node->_right = nullptr;

    if (node_cursor._parent == nullptr) {
      return;
    }

    fix_insert_violations(node);
  }

  // Removes the node referenced by the cursor
  // The cursor must point to a valid existing node
  void remove_at_cursor(const Cursor& node_cursor) {
    assert(node_cursor.valid());
    assert(node_cursor.found());
    _num_nodes--;

    IntrusiveRBNode* node = node_cursor.node();

    if (node->_left != nullptr && node->_right != nullptr) { // node has two children
      // Swap place with the in-order successor and delete there instead
      IntrusiveRBNode* curr = node->_right;
      while (curr->_left != nullptr) {
        curr = curr->_left;
      }

      if (_root == node) _root = curr;

      std::swap(curr->_left, node->_left);
      std::swap(curr->_parent, node->_parent); // Swaps parent and color

      // If node is curr's parent, parent and right pointers become invalid
      if (node->_right == curr) {
        node->_right = curr->_right;
        node->set_parent(curr);
        curr->_right = node;
      } else {
        std::swap(curr->_right, node->_right);
        node->parent()->replace_child(curr, node);
        curr->_right->set_parent(curr);
      }

      if (curr->parent() != nullptr) curr->parent()->replace_child(node, curr);
      curr->_left->set_parent(curr);

      if (node->_left != nullptr) node->_left->set_parent(node);
      if (node->_right != nullptr) node->_right->set_parent(node);
    }

    remove_from_tree(node);
  }

  // Replace the node referenced by the cursor with a new node.
  // The user must ensure that no tree properties are broken:
  // There must not exist any node with the new key
  // For all nodes with key < old_key, must also have key < new_key
  // For all nodes with key > old_key, must also have key > new_key
  void replace_at_cursor(NodeType* new_node, const Cursor& node_cursor) {
    assert(new_node != nullptr);
    assert(node_cursor.valid());
    assert(node_cursor.found());
    NodeType* old_node = node_cursor.node();
    if (old_node == new_node) {
      return;
    }

    *node_cursor._insert_location = new_node;
    new_node->_parent = old_node->_parent;

    if (new_node->is_left_child()) {
      assert(less_than(static_cast<const NodeType*>(new_node), static_cast<const NodeType*>(new_node->parent())));
    } else if (new_node->is_right_child()) {
      assert(less_than(static_cast<const NodeType*>(new_node->parent()), static_cast<const NodeType*>(new_node)));
    }

    new_node->_left = old_node->_left;
    new_node->_right = old_node->_right;
    if (new_node->_left != nullptr) {
      assert(less_than(static_cast<const NodeType*>(new_node->_left), static_cast<const NodeType*>(new_node)));
      new_node->_left->set_parent(new_node);
    }
    if (new_node->_right != nullptr) {
      assert(less_than(static_cast<const NodeType*>(new_node), static_cast<const NodeType*>(new_node->_right)));
      new_node->_right->set_parent(new_node);
    }
  }

  // Removes the given node from the tree.
  void remove(NodeType* node) {
    assert(node != nullptr);
    Cursor node_cursor = cursor(node);
    remove_at_cursor(node_cursor);
  }

  // Returns leftmost node, nullptr if tree is empty.
  const NodeType* leftmost() const {
    IntrusiveRBNode* node = _root;
    if (node == nullptr) {
      return nullptr;
    }
    while (node->_left != nullptr) {
      node = node->_left;
    }
    return static_cast<const NodeType*>(node);
  }

  NodeType* leftmost() {
    return const_cast<NodeType*>(static_cast<const AbstractRBTree*>(this)->leftmost());
  }

  // Returns rightmost node, nullptr if tree is empty.
  const NodeType* rightmost() const {
    IntrusiveRBNode* node = _root;
    if (node == nullptr) {
      return nullptr;
    }
    while (node->_right != nullptr) {
      node = node->_right;
    }
    return static_cast<const NodeType*>(node);
  }

  NodeType* rightmost() {
    return const_cast<NodeType*>(static_cast<const AbstractRBTree*>(this)->rightmost());
  }

  // Verifies that the tree is correct and holds the RB invariants.
  void verify_self() const {
    // HotSpot leaves a freshly inserted root red (fix_insert_violations only
    // recolors it when a red child appears), so root color is not asserted.
    size_t black_height = 0;
    const size_t count = verify_node(_root, black_height);
    assert(count == _num_nodes);
    (void)count;
    (void)black_height;
  }
};

template <typename K, typename COMPARATOR>
using IntrusiveRBTree = AbstractRBTree<K, IntrusiveRBNode, COMPARATOR>;

} // namespace MapleRuntime

#endif // MRT_BASE_RB_TREE_H
