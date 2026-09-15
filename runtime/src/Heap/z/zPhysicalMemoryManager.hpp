// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zPhysicalMemoryManager.hpp:38-77.

#pragma once
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zArray.hpp"
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zRange.hpp"
#include "Heap/z/zRangeRegistry.hpp"
#include "Heap/z/zValue.hpp"
#include "Heap/z/zPhysicalMemoryBacking_linux.hpp"

namespace MapleRuntime {

class ZVirtualMemory;

using ZBackingIndexRange = ZRange<zbacking_index, zbacking_index_end>;

class ZPhysicalMemoryManager {
private:
  using ZBackingIndexRegistry = ZRangeRegistry<ZBackingIndexRange>;

  ZPhysicalMemoryBacking          _backing;
  ZPerNUMA<ZBackingIndexRegistry> _partition_registries;
  ZGranuleMap<zbacking_index>     _physical_mappings;

  void copy_to_stash(ZArraySlice<zbacking_index> stash, const ZVirtualMemory& vmem) const;
  void copy_from_stash(const ZArraySlice<const zbacking_index> stash, const ZVirtualMemory& vmem);

public:
  ZPhysicalMemoryManager(size_t max_capacity);

  bool is_initialized() const;

  void warn_commit_limits(size_t max_capacity) const;
  void try_enable_uncommit(size_t min_capacity, size_t max_capacity);

  void alloc(const ZVirtualMemory& vmem, uint32_t numa_id);
  void free(const ZVirtualMemory& vmem, uint32_t numa_id);

  size_t commit(const ZVirtualMemory& vmem, uint32_t numa_id);
  size_t uncommit(const ZVirtualMemory& vmem);

  void map(const ZVirtualMemory& vmem, uint32_t numa_id) const;
  void unmap(const ZVirtualMemory& vmem) const;

  void copy_physical_segments(const ZVirtualMemory& to, const ZVirtualMemory& from);

  void sort_segments_physical(const ZVirtualMemory& vmem);

  void stash_segments(const ZVirtualMemory& vmem, ZArray<zbacking_index>* stash_out) const;
  void restore_segments(const ZVirtualMemory& vmem, const ZArray<zbacking_index>& stash);

  void stash_segments(const ZArraySlice<const ZVirtualMemory>& vmems, ZArray<zbacking_index>* stash_out) const;
  void restore_segments(const ZArraySlice<const ZVirtualMemory>& vmems, const ZArray<zbacking_index>& stash);
};

} // namespace MapleRuntime
