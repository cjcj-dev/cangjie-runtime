// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zRange.hpp:29-74.

#pragma once
#include <cstddef>

namespace MapleRuntime {

template <typename Start, typename End>
class ZRange {
public:
  using offset     = Start;
  using offset_end = End;

private:
  End    _start;
  size_t _size;

  // Used internally to create a ZRange.
  //
  // The end parameter is only used for verification and to distinguish
  // the constructors if End == Start.
  ZRange(End start, size_t size, End end);

public:
  ZRange();
  ZRange(Start start, size_t size);

  bool is_null() const;

  Start start() const;
  End end() const;

  size_t size() const;

  bool operator==(const ZRange& other) const;
  bool operator!=(const ZRange& other) const;

  bool contains(const ZRange& other) const;

  void grow_from_front(size_t size);
  void grow_from_back(size_t size);

  ZRange shrink_from_front(size_t size);
  ZRange shrink_from_back(size_t size);

  ZRange partition(size_t offset, size_t partition_size) const;
  ZRange first_part(size_t split_offset) const;
  ZRange last_part(size_t split_offset) const;

  bool adjacent_to(const ZRange& other) const;
};

} // namespace MapleRuntime
