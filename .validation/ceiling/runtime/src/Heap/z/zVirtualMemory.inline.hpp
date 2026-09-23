// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zVirtualMemory.inline.hpp:35-55. The granule is ZGranuleSize
// (one page-allocator unit) until P03/P05 move pages onto ZGranuleSize.

#pragma once
#include "Heap/z/zVirtualMemory.hpp"

#include <cassert>
#include <limits>

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zRange.inline.hpp"

namespace MapleRuntime {

inline ZVirtualMemory::ZVirtualMemory()
  : ZRange() {}

inline ZVirtualMemory::ZVirtualMemory(zoffset start, size_t size)
  : ZRange(start, size) {
  // ZVirtualMemory is only used for granule multiple ranges
  assert(untype(start) % ZGranuleSize == 0);
  assert(size % ZGranuleSize == 0);
}

inline ZVirtualMemory::ZVirtualMemory(const ZRange<zoffset, zoffset_end>& range)
  : ZVirtualMemory(range.start(), range.size()) {}

inline int ZVirtualMemory::granule_count() const {
  const size_t granule_count = (size() >> ZGranuleSizeShift);

  assert(granule_count <= static_cast<size_t>(std::numeric_limits<int>::max()));

  return static_cast<int>(granule_count);
}

} // namespace MapleRuntime
