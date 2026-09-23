// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zRange.inline.hpp:32-142.

#pragma once
#include "Heap/z/zRange.hpp"

#include <cassert>

#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {

template <typename Start, typename End>
inline ZRange<Start, End>::ZRange(End start, size_t size, End end)
  : _start(start),
    _size(size) {
  assert(this->end() == end);
  (void)end;
}

template <typename Start, typename End>
inline ZRange<Start, End>::ZRange()
  : _start(End::invalid),
    _size(0) {}

template <typename Start, typename End>
inline ZRange<Start, End>::ZRange(Start start, size_t size)
  : _start(to_end_type(start, 0)),
    _size(size) {}

template <typename Start, typename End>
inline bool ZRange<Start, End>::is_null() const {
  return _start == End::invalid;
}

template <typename Start, typename End>
inline Start ZRange<Start, End>::start() const {
  return to_start_type(_start);
}

template <typename Start, typename End>
inline End ZRange<Start, End>::end() const {
  return _start + _size;
}

template <typename Start, typename End>
inline size_t ZRange<Start, End>::size() const {
  return _size;
}

template <typename Start, typename End>
inline bool ZRange<Start, End>::operator==(const ZRange& other) const {
  assert(!is_null());
  assert(!other.is_null());

  return _start == other._start && _size == other._size;
}

template <typename Start, typename End>
inline bool ZRange<Start, End>::operator!=(const ZRange& other) const {
  return !operator==(other);
}

template <typename Start, typename End>
inline bool ZRange<Start, End>::contains(const ZRange& other) const {
  assert(!is_null());
  assert(!other.is_null());

  return _start <= other._start && other.end() <= end();
}

template <typename Start, typename End>
inline void ZRange<Start, End>::grow_from_front(size_t size) {
  assert(size_t(start()) >= size);

  _start -= size;
  _size  += size;
}

template <typename Start, typename End>
inline void ZRange<Start, End>::grow_from_back(size_t size) {
  _size += size;
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::shrink_from_front(size_t size) {
  assert(this->size() >= size);

  _start += size;
  _size  -= size;

  return ZRange(_start - size, size, _start);
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::shrink_from_back(size_t size) {
  assert(this->size() >= size);

  _size -= size;

  return ZRange(end(), size, end() + size);
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::partition(size_t offset, size_t partition_size) const {
  assert(size() - offset >= partition_size);

  return ZRange(_start + offset, partition_size, _start + offset + partition_size);
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::first_part(size_t split_offset) const {
  return partition(0, split_offset);
}

template <typename Start, typename End>
inline ZRange<Start, End> ZRange<Start, End>::last_part(size_t split_offset) const {
  return partition(split_offset, size() - split_offset);
}

template <typename Start, typename End>
inline bool ZRange<Start, End>::adjacent_to(const ZRange<Start, End>& other) const {
  return end() == other.start() || other.end() == start();
}

} // namespace MapleRuntime
