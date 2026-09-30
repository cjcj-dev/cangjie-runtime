// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {
struct ZMemoryUsage {
    size_t used;
    size_t current;
    size_t max;
};

struct ZMemoryUsageInfo {
    ZMemoryUsage young;
    ZMemoryUsage old;
};

// zServiceability.cpp:41-53,142-150. Both pools share the heap limit;
// committed capacity is partitioned with old occupancy taking precedence.
ZMemoryUsageInfo ComputeMemoryUsageInfo(size_t capacity, size_t maxCapacity,
                                      size_t youngUsed, size_t oldUsed);

// HotSpot memoryManager.hpp:134-176, memoryManager.cpp:222-281.
// Each collection kind owns its current usage and completed accumulated time.
class GCMemoryManager {
public:
    void gc_begin();
    void gc_end();
    uint64_t gc_time_us() const;
    size_t gc_freed_size() const;

private:
    mutable std::mutex _lock;
    uint64_t _start = 0;
    uint64_t _accumulated_time_ns = 0;
    ZMemoryUsageInfo _before_gc_usage {};
    ZMemoryUsageInfo _after_gc_usage {};
    // Cangjie cumulative API mapping of HotSpot before/after usage records.
    size_t _accumulated_freed = 0;
};

// HotSpot memoryService.cpp:241-291: scope owns the begin/end pairing.
class TraceMemoryManagerStats {
public:
    explicit TraceMemoryManagerStats(GCMemoryManager* manager);
    ~TraceMemoryManagerStats();
    TraceMemoryManagerStats(const TraceMemoryManagerStats&) = delete;
    TraceMemoryManagerStats& operator=(const TraceMemoryManagerStats&) = delete;
private:
    GCMemoryManager* _gc_memory_manager;
};

// ZGC zServiceability.hpp:55-79: separate minor and major cycle managers.
class ZServiceability {
public:
    GCMemoryManager* cycle_memory_manager(bool minor);
private:
    GCMemoryManager _minor_cycle_memory_manager;
    GCMemoryManager _major_cycle_memory_manager;
};

// ZGC zServiceability.cpp:202-220; held by each driver collection scope.
class ZServiceabilityCycleTracer {
public:
    explicit ZServiceabilityCycleTracer(bool minor);
private:
    TraceMemoryManagerStats _memory_manager_stats;
};
}
