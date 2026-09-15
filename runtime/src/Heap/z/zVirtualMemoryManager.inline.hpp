// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zVirtualMemoryManager.inline.hpp:32-49.

#pragma once
#include "Heap/z/zVirtualMemoryManager.hpp"

#include <cstdlib>

#include "Base/Log.h"
#include "Heap/z/zRangeRegistry.inline.hpp"

namespace MapleRuntime {

inline bool ZVirtualMemoryManager::is_multi_partition_enabled() const {
  return _is_multi_partition_enabled;
}

inline bool ZVirtualMemoryManager::is_in_multi_partition(const ZVirtualMemory& vmem) const {
  return _multi_partition_registry.limits_contain(vmem);
}

inline uint32_t ZVirtualMemoryManager::lookup_partition_id(const ZVirtualMemory& vmem) const {
  const uint32_t num_partitions = _partition_registries.count();
  for (uint32_t partition_id = 0; partition_id < num_partitions; partition_id++) {
    if (registry(partition_id).limits_contain(vmem)) {
      return partition_id;
    }
  }

  LOG(RTLOG_FATAL, "vmem outside every partition registry: start=%#zx size=%zu",
      static_cast<size_t>(vmem.start()), vmem.size());
  std::abort();
}

} // namespace MapleRuntime
