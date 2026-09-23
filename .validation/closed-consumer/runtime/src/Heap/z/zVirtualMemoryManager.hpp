// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zVirtualMemoryManager.hpp:33-108.

#pragma once
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zArray.hpp"
#include "Heap/z/zRangeRegistry.hpp"
#include "Heap/z/zValue.hpp"
#include "Heap/z/zVirtualMemory.hpp"

namespace MapleRuntime {

using ZVirtualMemoryRegistry = ZRangeRegistry<ZVirtualMemory>;

class ZVirtualMemoryReserver {
  friend class ZTest;
  friend class ZVirtualMemoryManager;
  friend class ZVirtualMemoryManagerTest;

private:

  ZVirtualMemoryRegistry _registry;
  const size_t           _reserved;

  static size_t calculate_min_range(size_t size);

  // Platform specific implementation
  void pd_register_callbacks(ZVirtualMemoryRegistry* registry);
  bool pd_reserve(zaddress_unsafe addr, size_t size);
  void pd_unreserve(zaddress_unsafe addr, size_t size);

  bool reserve_contiguous(zoffset start, size_t size);
  bool reserve_contiguous(size_t size);
  size_t reserve_discontiguous(zoffset start, size_t size, size_t min_range);
  size_t reserve_discontiguous(size_t size);

  size_t reserve(size_t size);
  void unreserve(const ZVirtualMemory& vmem);

public:
  ZVirtualMemoryReserver(size_t size);

  void initialize_partition_registry(ZVirtualMemoryRegistry* partition_registry, size_t size);

  void unreserve_all();

  bool is_empty() const;
  bool is_contiguous() const;

  size_t reserved() const;

  zoffset_end highest_available_address_end() const;
};

class ZVirtualMemoryManager {
private:
  ZPerNUMA<ZVirtualMemoryRegistry> _partition_registries;
  ZVirtualMemoryRegistry           _multi_partition_registry;
  bool                             _is_multi_partition_enabled;
  bool                             _initialized;

  ZVirtualMemoryRegistry& registry(uint32_t partition_id);
  const ZVirtualMemoryRegistry& registry(uint32_t partition_id) const;

public:
  ZVirtualMemoryManager(size_t max_capacity);
  ~ZVirtualMemoryManager();

  void initialize_partitions(ZVirtualMemoryReserver* reserver, size_t size_for_partitions);

  bool is_initialized() const;
  bool is_multi_partition_enabled() const;
  bool is_in_multi_partition(const ZVirtualMemory& vmem) const;

  uint32_t lookup_partition_id(const ZVirtualMemory& vmem) const;
  zoffset lowest_available_address(uint32_t partition_id) const;

  void insert(const ZVirtualMemory& vmem, uint32_t partition_id);
  void insert_multi_partition(const ZVirtualMemory& vmem);

  size_t remove_from_low_many_at_most(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out);
  ZVirtualMemory remove_from_low(size_t size, uint32_t partition_id);
  ZVirtualMemory remove_from_low_multi_partition(size_t size);

  void insert_and_remove_from_low_many(const ZVirtualMemory& vmem, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_out);
  ZVirtualMemory insert_and_remove_from_low_exact_or_many(size_t size, uint32_t partition_id, ZArray<ZVirtualMemory>* vmems_in_out);
};

} // namespace MapleRuntime
