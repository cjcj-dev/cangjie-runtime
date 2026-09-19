// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zVirtualMemoryManager.cpp:39-358. ZNMT registration has no counterpart
// (no native memory tracker, PLAN infra I16). ZForceDiscontiguousHeapReservations
// is a HotSpot debug flag and is not carried.

#include "Heap/z/zVirtualMemoryManager.inline.hpp"

#include <algorithm>

#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zValue.inline.hpp"
#include "Heap/z/zAddressSpaceLimit.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zNUMA.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

namespace MapleRuntime {

ZVirtualMemoryReserver::ZVirtualMemoryReserver(size_t size)
  : _registry(),
    _reserved(reserve(size)) {}

void ZVirtualMemoryReserver::initialize_partition_registry(ZVirtualMemoryRegistry* partition_registry, size_t size) {
  assert(partition_registry->is_empty());

  // Registers the Windows callbacks
  pd_register_callbacks(partition_registry);

  _registry.transfer_from_low(partition_registry, size);

  // Set the limits according to the virtual memory given to this partition
  partition_registry->anchor_limits();
}

void ZVirtualMemoryReserver::unreserve(const ZVirtualMemory& vmem) {
  const zaddress_unsafe addr = ZOffset::address_unsafe(vmem.start());

  // Unreserve address space
  pd_unreserve(addr, vmem.size());
}

void ZVirtualMemoryReserver::unreserve_all() {
  for (ZVirtualMemory vmem; _registry.unregister_first(&vmem);) {
    unreserve(vmem);
  }
}

bool ZVirtualMemoryReserver::is_empty() const {
  return _registry.is_empty();
}

bool ZVirtualMemoryReserver::is_contiguous() const {
  return _registry.is_contiguous();
}

size_t ZVirtualMemoryReserver::reserved() const {
  return _reserved;
}

zoffset_end ZVirtualMemoryReserver::highest_available_address_end() const {
  return _registry.peak_high_address_end();
}

size_t ZVirtualMemoryReserver::reserve_discontiguous(zoffset start, size_t size, size_t min_range) {
  if (size < min_range) {
    // Too small
    return 0;
  }

  assert(size % ZGranuleSize == 0);

  if (reserve_contiguous(start, size)) {
    return size;
  }

  const size_t half = size / 2;
  if (half < min_range) {
    // Too small
    return 0;
  }

  // Divide and conquer
  const size_t first_part = AlignDown(half, ZGranuleSize);
  const size_t second_part = size - first_part;
  const size_t first_size = reserve_discontiguous(start, first_part, min_range);
  const size_t second_size = reserve_discontiguous(start + first_part, second_part, min_range);
  return first_size + second_size;
}

size_t ZVirtualMemoryReserver::calculate_min_range(size_t size) {
  // Don't try to reserve address ranges smaller than 1% of the requested size.
  // This avoids an explosion of reservation attempts in case large parts of the
  // address space is already occupied.
  return AlignUp(size / ZMaxVirtualReservations, ZGranuleSize);
}

size_t ZVirtualMemoryReserver::reserve_discontiguous(size_t size) {
  const size_t min_range = calculate_min_range(size);
  uintptr_t start = 0;
  size_t reserved = 0;

  // Reserve size somewhere between [0, ZAddressOffsetMax)
  while (reserved < size && start < ZAddressOffsetMax) {
    const size_t remaining = std::min(size - reserved, ZAddressOffsetMax - start);
    reserved += reserve_discontiguous(to_zoffset(start), remaining, min_range);
    start += remaining;
  }

  return reserved;
}

bool ZVirtualMemoryReserver::reserve_contiguous(zoffset start, size_t size) {
  assert(size % ZGranuleSize == 0);

  // Reserve address views
  const zaddress_unsafe addr = ZOffset::address_unsafe(start);

  // Reserve address space
  if (!pd_reserve(addr, size)) {
    return false;
  }

  // Register the memory reservation
  _registry.register_range({start, size});

  return true;
}

bool ZVirtualMemoryReserver::reserve_contiguous(size_t size) {
  // Allow at most 8192 attempts spread evenly across [0, ZAddressOffsetMax)
  const size_t unused = ZAddressOffsetMax - size;
  const size_t increment = std::max(AlignUp(unused / 8192, ZGranuleSize), ZGranuleSize);

  for (uintptr_t start = 0; start + size <= ZAddressOffsetMax; start += increment) {
    if (reserve_contiguous(to_zoffset(start), size)) {
      // Success
      return true;
    }
  }

  // Failed
  return false;
}

size_t ZVirtualMemoryReserver::reserve(size_t size) {
  // Register Windows callbacks
  pd_register_callbacks(&_registry);

  if (size == 0) {
    // Nothing to reserve (~ZVirtualMemoryManager builds an empty reserver
    // to hand address space back)
    return 0;
  }

  // Reserve address space

  // Prefer a contiguous address space
  if (reserve_contiguous(size)) {
    return size;
  }

  // Fall back to a discontiguous address space
  return reserve_discontiguous(size);
}

ZVirtualMemoryManager::ZVirtualMemoryManager(size_t max_capacity)
  : _partition_registries(),
    _multi_partition_registry(),
    _is_multi_partition_enabled(false),
    _initialized(false) {

  assert(max_capacity <= ZAddressOffsetMax);

  ZAddressSpaceLimit::print_limits();

  const size_t limit = std::min(ZAddressOffsetMax, ZAddressSpaceLimit::heap());

  const size_t desired_for_partitions = max_capacity * ZVirtualToPhysicalRatio;
  // Multi-partition address space (ZNUMA::count() > 1) is A03n.
  const size_t desired_for_multi_partition = 0;

  const size_t desired = desired_for_partitions + desired_for_multi_partition;
  const size_t requested = desired <= limit
      ? desired
      : std::min(desired_for_partitions, limit);

  // Reserve virtual memory for the heap
  ZVirtualMemoryReserver reserver(requested);

  const size_t reserved = reserver.reserved();
  const bool is_contiguous = reserver.is_contiguous();

  VLOG(REPORT, "Reserved Space: limit %zuM, desired %zuM, requested %zuM", limit / MB, desired / MB, requested / MB);

  if (reserved < max_capacity) {
    LOG(RTLOG_ERROR, "Failed to reserve %zuM address space for Cangjie heap", max_capacity / MB);
    reserver.unreserve_all();
    return;
  }

  // Set ZAddressOffsetMax to the highest address end available after reservation
  ZAddressOffsetMax = untype(reserver.highest_available_address_end());

  const size_t size_for_partitions = std::min(reserved, desired_for_partitions);

  // Divide size_for_partitions virtual memory over the NUMA nodes
  initialize_partitions(&reserver, size_for_partitions);

  // Set up multi-partition or unreserve the surplus memory
  if (desired_for_multi_partition > 0 && reserved == desired) {
    // Enough left to setup the multi-partition memory reservation
    reserver.initialize_partition_registry(&_multi_partition_registry, desired_for_multi_partition);
    _is_multi_partition_enabled = true;
  } else {
    // Failed to reserve enough memory for multi-partition, unreserve unused memory
    reserver.unreserve_all();
  }

  assert(reserver.is_empty());

  VLOG(REPORT, "Reserved Space Type: %s/%s/%s",
       (is_contiguous ? "Contiguous" : "Discontiguous"),
       (requested == desired ? "Unrestricted" : "Restricted"),
       (reserved == desired ? "Complete" : ((reserved < desired_for_partitions) ? "Degraded"  : "NUMA-Degraded")));
  VLOG(REPORT, "Reserved Space Size: %zuM", reserved / MB);

  // Successfully initialized
  _initialized = true;
}

ZVirtualMemoryManager::~ZVirtualMemoryManager() {
  // Hand the reserved address space back. ZGC keeps its heap for the whole
  // VM lifetime; gtests here construct and destroy managers.
  ZVirtualMemoryReserver reserver(0);
  uint32_t partition_id;
  ZPerNUMAIterator<ZVirtualMemoryRegistry> iter(&_partition_registries);
  for (ZVirtualMemoryRegistry* registry; iter.next(&registry, &partition_id);) {
    for (ZVirtualMemory vmem; registry->unregister_first(&vmem);) {
      reserver.unreserve(vmem);
    }
  }
  for (ZVirtualMemory vmem; _multi_partition_registry.unregister_first(&vmem);) {
    reserver.unreserve(vmem);
  }
}

void ZVirtualMemoryManager::initialize_partitions(ZVirtualMemoryReserver* reserver, size_t size_for_partitions) {
  assert(size_for_partitions % ZGranuleSize == 0);

  const uint32_t numa_count = _partition_registries.count();

  // If the capacity consist of less granules than the number of partitions
  // some partitions will be empty. Distribute these shares on the none empty
  // partitions.
  const uint32_t first_empty_numa_id = std::min(static_cast<uint32_t>((size_for_partitions >> ZGranuleSizeShift)), numa_count);
  const uint32_t ignore_count = numa_count - first_empty_numa_id;

  // Install reserved memory into registry(s)
  uint32_t numa_id;
  ZPerNUMAIterator<ZVirtualMemoryRegistry> iter(&_partition_registries);
  for (ZVirtualMemoryRegistry* registry; iter.next(&registry, &numa_id);) {
    if (numa_id == first_empty_numa_id) {
      break;
    }

    // Calculate how much reserved memory this partition gets
    const size_t reserved_for_partition = NumaTopology::calculate_share(numa_id, size_for_partitions, ZGranuleSize, ignore_count);

    // Transfer reserved memory
    reserver->initialize_partition_registry(registry, reserved_for_partition);
  }
}

bool ZVirtualMemoryManager::is_initialized() const {
  return _initialized;
}

ZVirtualMemoryRegistry& ZVirtualMemoryManager::registry(uint32_t partition_id) {
  return _partition_registries.get(partition_id);
}

const ZVirtualMemoryRegistry& ZVirtualMemoryManager::registry(uint32_t partition_id) const {
  return _partition_registries.get(partition_id);
}

zoffset ZVirtualMemoryManager::lowest_available_address(uint32_t partition_id) const {
  return registry(partition_id).peek_low_address();
}

void ZVirtualMemoryManager::insert(const ZVirtualMemory& vmem, uint32_t partition_id) {
  assert(partition_id == lookup_partition_id(vmem));
  registry(partition_id).insert(vmem);
}

void ZVirtualMemoryManager::insert_multi_partition(const ZVirtualMemory& vmem) {
  _multi_partition_registry.insert(vmem);
}

size_t ZVirtualMemoryManager::remove_from_low_many_at_most(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out) {
  return registry(partition_id).remove_from_low_many_at_most(size, vmems_out);
}

ZVirtualMemory ZVirtualMemoryManager::remove_from_low(size_t size, uint32_t partition_id) {
  return registry(partition_id).remove_from_low(size);
}

ZVirtualMemory ZVirtualMemoryManager::remove_from_low_multi_partition(size_t size) {
  return _multi_partition_registry.remove_from_low(size);
}

void ZVirtualMemoryManager::insert_and_remove_from_low_many(const ZVirtualMemory& vmem, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out) {
  registry(partition_id).insert_and_remove_from_low_many(vmem, vmems_out);
}

ZVirtualMemory ZVirtualMemoryManager::insert_and_remove_from_low_exact_or_many(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_in_out) {
  return registry(partition_id).insert_and_remove_from_low_exact_or_many(size, vmems_in_out);
}

} // namespace MapleRuntime
