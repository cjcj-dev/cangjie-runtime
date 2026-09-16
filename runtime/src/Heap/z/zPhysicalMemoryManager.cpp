// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zPhysicalMemoryManager.cpp:48-393. ZNMT registration (infra I16) and
// the ZFailLargerCommits diagnostic flag are not carried; ZUncommit /
// ZUncommitDelay are read from the environment (infra I15).

#include "Heap/z/zPhysicalMemoryManager.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Heap/z/zValue.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zGranuleMap.inline.hpp"
#include "Heap/z/zLargePages.inline.hpp"
#include "Heap/z/zList.inline.hpp"
#include "Heap/z/zNUMA.inline.hpp"
#include "Heap/z/zRangeRegistry.inline.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {

ZPhysicalMemoryManager::ZPhysicalMemoryManager(size_t max_capacity)
  : _backing(max_capacity),
    _physical_mappings(ZAddressOffsetMax, 0, ZBackingGranuleSize) {
  assert(max_capacity % ZBackingGranuleSize == 0);

  // Setup backing storage limits
  ZBackingOffsetMax = max_capacity;
  ZBackingIndexMax = static_cast<uint32_t>(max_capacity / ZBackingGranuleSize);

  // Install capacity into the registry
  const size_t num_segments_total = max_capacity / ZBackingGranuleSize;
  zbacking_index_end next_index = zbacking_index_end::zero;
  uint32_t numa_id;
  ZPerNUMAIterator<ZBackingIndexRegistry> iter(&_partition_registries);
  for (ZBackingIndexRegistry* registry; iter.next(&registry, &numa_id);) {
    const size_t num_segments = NumaTopology::calculate_share(numa_id, num_segments_total, 1 /* granule */);

    if (num_segments == 0) {
      // If the capacity consist of less granules than the number of partitions,
      // some partitions will be empty.
      break;
    }

    const zbacking_index index = to_zbacking_index(next_index);

    // Insert the next number of segment indices into id's partition's registry
    registry->insert({index, num_segments});

    // Advance to next index by the inserted number of segment indices
    next_index += num_segments;
  }

  assert(untype(next_index) == ZBackingIndexMax);
}

bool ZPhysicalMemoryManager::is_initialized() const {
  return _backing.is_initialized();
}

void ZPhysicalMemoryManager::warn_commit_limits(size_t max_capacity) const {
  _backing.warn_commit_limits(max_capacity);
}

void ZPhysicalMemoryManager::try_enable_uncommit(size_t min_capacity, size_t max_capacity) {
  // If uncommit is not explicitly disabled, max capacity is greater than
  // min capacity, and uncommit is supported by the platform, then uncommit
  // will be enabled.
  if (!Uncommitter::ZUncommit()) {
    VLOG(REPORT, "Uncommit: Disabled");
    return;
  }

  if (max_capacity == min_capacity) {
    VLOG(REPORT, "Uncommit: Implicitly Disabled (initial heap equals max heap)");
    Uncommitter::SetZUncommit(false);
    return;
  }

  // Test if uncommit is supported by the operating system by committing
  // and then uncommitting a granule.
  const ZVirtualMemory vmem(zoffset(0), ZBackingGranuleSize);
  if (!commit(vmem, 0) || !uncommit(vmem)) {
    VLOG(REPORT, "Uncommit: Implicitly Disabled (Not supported by operating system)");
    Uncommitter::SetZUncommit(false);
    return;
  }

  const size_t max_delay_without_overflow = std::numeric_limits<uint64_t>::max() / 1000;
  if (Uncommitter::ZUncommitDelay() > max_delay_without_overflow) {
    Uncommitter::SetZUncommitDelay(max_delay_without_overflow);
  }

  VLOG(REPORT, "Uncommit: Enabled");
  VLOG(REPORT, "Uncommit Delay: %zus", Uncommitter::ZUncommitDelay());
}

void ZPhysicalMemoryManager::alloc(const ZVirtualMemory& vmem, uint32_t numa_id) {
  zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const size_t size = vmem.size();

  assert(size % ZBackingGranuleSize == 0);

  size_t current_segment = 0;
  size_t remaining_segments = size / ZBackingGranuleSize;

  while (remaining_segments != 0) {
    // Allocate a range of backing segment indices
    ZBackingIndexRegistry& registry = _partition_registries.get(numa_id);
    const ZBackingIndexRange range = registry.remove_from_low_at_most(remaining_segments);
    CHECK_DETAIL(!range.is_null(), "backing index allocation must never fail");

    const size_t num_allocated_segments = range.size();

    // Insert backing segment indices in pmem
    const zbacking_index start_i = range.start();
    for (size_t i = 0; i < num_allocated_segments; i++) {
      pmem[current_segment + i] = start_i + i;
    }

    // Advance by number of allocated segments
    remaining_segments -= num_allocated_segments;
    current_segment += num_allocated_segments;
  }
}

template <typename ReturnType>
struct IterateInvoker {
  template<typename Function>
  bool operator()(Function function, zbacking_offset segment_start, size_t segment_size) const {
    return function(segment_start, segment_size);
  }
};

template<>
struct IterateInvoker<void> {
  template<typename Function>
  bool operator()(Function function, zbacking_offset segment_start, size_t segment_size) const {
    function(segment_start, segment_size);
    return true;
  }
};

template<typename Function>
bool for_each_segment_apply(const zbacking_index* pmem, size_t size, Function function) {
  IterateInvoker<decltype(function(zbacking_offset{}, size_t{}))> invoker;

  // Total number of segment indices
  const size_t num_segments = size / ZBackingGranuleSize;

  // Apply the function over all zbacking_offset ranges consisting of consecutive indices
  for (size_t i = 0; i < num_segments; i++) {
    const size_t start_i = i;

    // Find index corresponding to the last index in the consecutive range starting at start_i
    while (i + 1 < num_segments && to_zbacking_index_end(pmem[i], 1) == pmem[i + 1]) {
      i++;
    }

    const size_t last_i = i;

    // [start_i, last_i] now forms a consecutive range of indicies in pmem
    const size_t num_indicies = last_i - start_i + 1;
    const zbacking_offset start = to_zbacking_offset(pmem[start_i]);
    const size_t size = num_indicies * ZBackingGranuleSize;

    // Invoke function on zbacking_offset Range [start, start + size[
    if (!invoker(function, start, size)) {
      return false;
    }
  }

  return true;
}

void ZPhysicalMemoryManager::free(const ZVirtualMemory& vmem, uint32_t numa_id) {
  zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const size_t size = vmem.size();

  // Free segments
  for_each_segment_apply(pmem, size, [&](zbacking_offset segment_start, size_t segment_size) {
    const size_t num_segments = segment_size / ZBackingGranuleSize;
    const zbacking_index index = to_zbacking_index(segment_start);

    // Insert the free segment indices
    _partition_registries.get(numa_id).insert({index, num_segments});
  });
}

size_t ZPhysicalMemoryManager::commit(const ZVirtualMemory& vmem, uint32_t numa_id) {
  zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const size_t size = vmem.size();

  size_t total_committed = 0;

  // Commit segments
  for_each_segment_apply(pmem, size, [&](zbacking_offset segment_start, size_t segment_size) {
    // Commit segment
    const size_t committed = _backing.commit(segment_start, segment_size, numa_id);

    total_committed += committed;

    return segment_size == committed;
  });

  // Success
  return total_committed;
}

size_t ZPhysicalMemoryManager::uncommit(const ZVirtualMemory& vmem) {
  zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const size_t size = vmem.size();

  size_t total_uncommitted = 0;

  // Uncommit segments
  for_each_segment_apply(pmem, size, [&](zbacking_offset segment_start, size_t segment_size) {
    // Uncommit segment
    const size_t uncommitted = _backing.uncommit(segment_start, segment_size);

    total_uncommitted += uncommitted;

    return segment_size == uncommitted;
  });

  // Success
  return total_uncommitted;
}

// Map virtual memory to physical memory
void ZPhysicalMemoryManager::map(const ZVirtualMemory& vmem, uint32_t numa_id) const {
  const zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const zaddress_unsafe addr = ZOffset::address_unsafe(vmem.start());
  const size_t size = vmem.size();

  size_t mapped = 0;

  for_each_segment_apply(pmem, size, [&](zbacking_offset segment_start, size_t segment_size) {
    _backing.map(to_zaddress_unsafe(untype(addr) + mapped), segment_size, segment_start);
    mapped += segment_size;
  });

  assert(mapped == size);

  // Setup NUMA preferred for large pages (os::numa_make_local): explicit
  // large pages with more than one NUMA node. A03n (static ZNUMA) owns the
  // numa_id → node conversion; the sealed topology supplies it until then.
  if (ZPerNUMAStorage::count() > 1 && ZLargePages::is_explicit()) {
    NumaTopology::numa_make_local(reinterpret_cast<void*>(untype(addr)), size, numa_id);
  }
}

// Unmap virtual memory from physical memory
void ZPhysicalMemoryManager::unmap(const ZVirtualMemory& vmem) const {
  const zaddress_unsafe addr = ZOffset::address_unsafe(vmem.start());
  const size_t size = vmem.size();
  _backing.unmap(addr, size);
}

void ZPhysicalMemoryManager::copy_physical_segments(const ZVirtualMemory& to, const ZVirtualMemory& from) {
  assert(to.size() == from.size());

  zbacking_index* const dest = _physical_mappings.addr(to.start());
  const zbacking_index* const src = _physical_mappings.addr(from.start());
  const int granule_count = from.granule_count();

  ZUtils::copy_disjoint(dest, src, granule_count);
}

static void sort_zbacking_index_array(zbacking_index* array, int count) {
  ZUtils::sort(array, count, [](const zbacking_index* e1, const zbacking_index* e2) {
    return *e1 < *e2 ? -1 : 1;
  });
}

void ZPhysicalMemoryManager::sort_segments_physical(const ZVirtualMemory& vmem) {
  zbacking_index* const pmem = _physical_mappings.addr(vmem.start());
  const int granule_count = vmem.granule_count();

  // Sort physical segments
  sort_zbacking_index_array(pmem, granule_count);
}

void ZPhysicalMemoryManager::copy_to_stash(ZArraySlice<zbacking_index> stash, const ZVirtualMemory& vmem) const {
  zbacking_index* const dest = stash.adr_at(0);
  const zbacking_index* const src = _physical_mappings.addr(vmem.start());
  const int granule_count = vmem.granule_count();

  // Check bounds
  assert(granule_count <= stash.length());

  // Copy to stash
  ZUtils::copy_disjoint(dest, src, granule_count);
}

void ZPhysicalMemoryManager::copy_from_stash(const ZArraySlice<const zbacking_index> stash, const ZVirtualMemory& vmem) {
  zbacking_index* const dest = _physical_mappings.addr(vmem.start());
  const zbacking_index* const src = stash.adr_at(0);
  const int granule_count = vmem.granule_count();

  // Check bounds
  assert(granule_count <= stash.length());

  // Copy from stash
  ZUtils::copy_disjoint(dest, src, granule_count);
}

void ZPhysicalMemoryManager::stash_segments(const ZVirtualMemory& vmem, ZArray<zbacking_index>* stash_out) const {
  assert(stash_out->is_empty());

  stash_out->at_grow(vmem.granule_count() - 1);
  copy_to_stash(*stash_out, vmem);
  sort_zbacking_index_array(stash_out->adr_at(0), stash_out->length());
}

void ZPhysicalMemoryManager::restore_segments(const ZVirtualMemory& vmem, const ZArray<zbacking_index>& stash) {
  assert(vmem.granule_count() == stash.length());

  copy_from_stash(stash, vmem);
}

void ZPhysicalMemoryManager::stash_segments(const ZArraySlice<const ZVirtualMemory>& vmems, ZArray<zbacking_index>* stash_out) const {
  assert(stash_out->is_empty());

  int stash_index = 0;
  for (const ZVirtualMemory& vmem : vmems) {
    const int granule_count = vmem.granule_count();
    stash_out->at_grow(stash_index + vmem.granule_count() - 1);
    copy_to_stash(stash_out->slice_back(stash_index), vmem);
    stash_index += granule_count;
  }

  sort_zbacking_index_array(stash_out->adr_at(0), stash_out->length());

}

void ZPhysicalMemoryManager::restore_segments(const ZArraySlice<const ZVirtualMemory>& vmems, const ZArray<zbacking_index>& stash) {
  int stash_index = 0;

  for (const ZVirtualMemory& vmem : vmems) {
    copy_from_stash(stash.slice_back(stash_index), vmem);
    stash_index += vmem.granule_count();
  }

  assert(stash_index == stash.length());
}

} // namespace MapleRuntime
