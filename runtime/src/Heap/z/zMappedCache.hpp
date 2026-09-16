// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zMappedCache.hpp:35-142. Entries live inside the cached memory itself
// (zMappedCache.cpp:92-119); the tree is the intrusive red-black tree
// (utilities/rbTree.hpp) and the size class lists are ZList<>. Size classes
// are keyed by the manager granule (ZBackingGranuleSize) rather than
// ZGranuleSize until P03/P05 move pages onto 2MB granules; the list array is
// sized for the smallest supported granule (4KB), classes above the runtime
// MaxSizeClassShift stay unused.

#pragma once
#include <cstddef>
#include <cstdint>

#include "Base/RBTree.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zArray.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zList.hpp"
#include "Heap/z/zVirtualMemory.hpp"

namespace MapleRuntime {

class ZMappedCacheEntry;

class ZMappedCache {
  friend class ZMappedCacheEntry;

private:
  struct EntryCompare {
    static RBTreeOrdering cmp(zoffset a, const IntrusiveRBNode* b);
    static bool less_than(const IntrusiveRBNode*  a, const IntrusiveRBNode* b);
  };

  struct ZSizeClassListNode {
    ZListNode<ZSizeClassListNode> _node;
  };

  using TreeImpl          = IntrusiveRBTree<zoffset, EntryCompare>;
  using TreeCursor        = TreeImpl::Cursor;
  using TreeNode          = IntrusiveRBNode;
  using SizeClassList     = ZList<ZSizeClassListNode>;
  using SizeClassListNode = ZSizeClassListNode;

  class Tree : private TreeImpl {
  private:
    TreeNode* _left_most;
    TreeNode* _right_most;

    void verify() const;
    void verify_left_most() const;
    void verify_right_most() const;

  public:
    Tree();

    void insert(TreeNode* node, const TreeCursor& cursor);
    void remove(TreeNode* node);
    void replace(TreeNode* old_node, TreeNode* new_node, const TreeCursor& cursor);

    size_t size_atomic() const;
    using TreeImpl::size;

    using TreeImpl::cursor;
    using TreeImpl::next;

    const TreeNode* left_most() const;
    TreeNode* left_most();

    const TreeNode* right_most() const;
    TreeNode* right_most();
  };

  // Maintain size class lists from 2 granules to 16GB
  static constexpr int MaxLongArraySizeClassShift = 3 /* 8 byte */ + 31 /* max length */;
  static constexpr int MinSizeClassShift = 1;
  static constexpr int MinGranuleSizeShift = 12; // smallest supported ZBackingGranuleSize
  static constexpr int NumSizeClasses = MaxLongArraySizeClassShift - MinGranuleSizeShift - MinSizeClassShift + 1;

  Tree          _tree;
  SizeClassList _size_class_lists[NumSizeClasses];
  size_t        _size;
  size_t        _min_size_watermark;

  static int granule_size_shift();
  static int max_size_class_shift();
  static int size_class_index(size_t size);
  static int guaranteed_size_class_index(size_t size);

  void cache_insert(const TreeCursor& cursor, const ZVirtualMemory& vmem);
  void cache_remove(const TreeCursor& cursor, const ZVirtualMemory& vmem);
  void cache_replace(const TreeCursor& cursor, const ZVirtualMemory& vmem);
  void cache_update(ZMappedCacheEntry* entry, const ZVirtualMemory& vmem);

  enum class RemovalStrategy {
    LowestAddress,
    HighestAddress,
    SizeClasses,
  };

  template <RemovalStrategy strategy, typename SelectFunction>
  ZVirtualMemory remove_vmem(ZMappedCacheEntry* const entry, size_t min_size, SelectFunction select);

  template <typename SelectFunction, typename ConsumeFunction>
  bool try_remove_vmem_size_class(size_t min_size, SelectFunction select, ConsumeFunction consume);

  template <RemovalStrategy strategy, typename SelectFunction, typename ConsumeFunction>
  void scan_remove_vmem(size_t min_size, SelectFunction select, ConsumeFunction consume);

  template <RemovalStrategy strategy, typename SelectFunction, typename ConsumeFunction>
  void scan_remove_vmem(SelectFunction select, ConsumeFunction consume);

  template <RemovalStrategy strategy>
  size_t remove_discontiguous_with_strategy(size_t size, ZArray<ZVirtualMemory>* out);

public:
  ZMappedCache();
  ~ZMappedCache();

  void insert(const ZVirtualMemory& vmem);

  ZVirtualMemory remove_contiguous(size_t size);
  ZVirtualMemory remove_contiguous_power_of_2(size_t min_size, size_t max_size);
  size_t remove_discontiguous(size_t size, ZArray<ZVirtualMemory>* out);

  // ZUncommitter support
  void reset_min_size_watermark();
  size_t min_size_watermark();
  size_t remove_for_uncommit(size_t size, ZArray<ZVirtualMemory>* out);

  void print_on() const;
  void print_extended_on() const;
};

} // namespace MapleRuntime
