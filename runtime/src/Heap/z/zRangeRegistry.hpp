// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zRangeRegistry.hpp:36-148.

#pragma once
#include <cstddef>
#include <mutex>

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zArray.hpp"
#include "Heap/z/zList.hpp"

namespace MapleRuntime {

template <typename Range>
class ZRangeRegistry {
  friend class ZVirtualMemoryManagerTest;

private:
  // The node type for the list of Ranges
  class Node;

public:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;

  typedef void (*CallbackPrepare)(const Range& range);
  typedef void (*CallbackResize)(const Range& from, const Range& to);

  struct Callbacks {
    CallbackPrepare _prepare_for_hand_out;
    CallbackPrepare _prepare_for_hand_back;
    CallbackResize  _grow;
    CallbackResize  _shrink;

    Callbacks();
  };

private:
  mutable std::mutex _lock;
  ZList<Node>        _list;
  Callbacks          _callbacks;
  Range              _limits;

  void move_into(const Range& range);

  void insert_inner(const Range& range);
  void register_inner(const Range& range);

  void grow_from_front(Range* range, size_t size);
  void grow_from_back(Range* range, size_t size);

  Range shrink_from_front(Range* range, size_t size);
  Range shrink_from_back(Range* range, size_t size);

  Range remove_from_low_inner(size_t size);
  Range remove_from_low_at_most_inner(size_t size);

  size_t remove_from_low_many_at_most_inner(size_t size, ZArray<Range>* out);

  bool check_limits(const Range& range) const;

public:
  ZRangeRegistry();
  ~ZRangeRegistry();

  void register_callbacks(const Callbacks& callbacks);

  void register_range(const Range& range);
  bool unregister_first(Range* out);

  bool is_empty() const;
  bool is_contiguous() const;

  void anchor_limits();
  bool limits_contain(const Range& range) const;

  offset peek_low_address() const;
  offset_end peak_high_address_end() const;

  void insert(const Range& range);

  void insert_and_remove_from_low_many(const Range& range, ZArray<Range>* out);
  Range insert_and_remove_from_low_exact_or_many(size_t size, ZArray<Range>* in_out);

  Range remove_from_low(size_t size);
  Range remove_from_low_at_most(size_t size);
  size_t remove_from_low_many_at_most(size_t size, ZArray<Range>* out);
  Range remove_from_high(size_t size);

  void transfer_from_low(ZRangeRegistry* other, size_t size);
};

template <typename Range>
class ZRangeRegistry<Range>::Node {
  friend class ZList<Node>;

private:
  using offset     = typename Range::offset;
  using offset_end = typename Range::offset_end;

  Range           _range;
  ZListNode<Node> _node;

public:
  Node(offset start, size_t size)
    : _range(start, size),
      _node() {}

  Node(const Range& other)
    : Node(other.start(), other.size()) {}

  Range* range() {
    return &_range;
  }

  offset start() const {
    return _range.start();
  }

  offset_end end() const {
    return _range.end();
  }

  size_t size() const {
    return _range.size();
  }
};

} // namespace MapleRuntime
