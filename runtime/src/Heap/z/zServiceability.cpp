// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zServiceability.hpp"
#include <algorithm>
#include "Base/TimeUtils.h"
#include "Heap/z/zHeap.hpp"

namespace MapleRuntime {
ZMemoryUsageInfo ComputeMemoryUsageInfo(size_t capacity, size_t maxCapacity,
                                      size_t youngUsed, size_t oldUsed)
{
    const size_t oldCapacity = std::min(oldUsed, capacity);
    const size_t youngCapacity = capacity - oldCapacity;
    return {{std::min(youngUsed, youngCapacity), youngCapacity, maxCapacity},
            {oldCapacity, oldCapacity, maxCapacity}};
}
}

namespace MapleRuntime {
void GCMemoryManager::gc_begin()
{
    std::lock_guard<std::mutex> guard(_lock);
    _start = TimeUtil::NanoSeconds();
    _before_gc_usage = Heap::GetHeap().GetMemoryUsage();
}

void GCMemoryManager::gc_end()
{
    std::lock_guard<std::mutex> guard(_lock);
    _accumulated_time_ns += TimeUtil::NanoSeconds() - _start;
    _after_gc_usage = Heap::GetHeap().GetMemoryUsage();
    const size_t before = _before_gc_usage.young.used + _before_gc_usage.old.used;
    const size_t after = _after_gc_usage.young.used + _after_gc_usage.old.used;
    // HotSpot management.cpp:1915-1936 exposes before/after pool usage.
    // Cangjie's cumulative freed-byte consumer accumulates positive net drops;
    // concurrent mutator allocation may make a collection's net drop zero.
    _accumulated_freed += before > after ? before - after : 0;
}

uint64_t GCMemoryManager::gc_time_us() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _accumulated_time_ns / 1000;
}

size_t GCMemoryManager::gc_freed_size() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _accumulated_freed;
}

TraceMemoryManagerStats::TraceMemoryManagerStats(GCMemoryManager* manager)
    : _gc_memory_manager(manager)
{
    _gc_memory_manager->gc_begin();
}

TraceMemoryManagerStats::~TraceMemoryManagerStats()
{
    _gc_memory_manager->gc_end();
}

GCMemoryManager* ZServiceability::cycle_memory_manager(bool minor)
{
    return minor ? &_minor_cycle_memory_manager : &_major_cycle_memory_manager;
}

ZServiceabilityCycleTracer::ZServiceabilityCycleTracer(bool minor)
    : _memory_manager_stats(Heap::GetHeap().serviceability_cycle_memory_manager(minor))
{}
}
