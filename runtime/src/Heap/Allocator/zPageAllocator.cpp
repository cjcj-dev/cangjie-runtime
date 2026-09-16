// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
namespace RecentFullAccounting {
namespace {
std::atomic<size_t> enqueuedRegions{ 0 };
std::atomic<size_t> dequeuedRegions{ 0 };
std::atomic<size_t> currentBytes{ 0 };
std::atomic<size_t> peakBytes{ 0 };
}

void Enqueue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    enqueuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * ZPage::UNIT_SIZE;
    const size_t current = currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    size_t peak = peakBytes.load(std::memory_order_relaxed);
    while (peak < current &&
           !peakBytes.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {}
}

void Dequeue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    dequeuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * ZPage::UNIT_SIZE;
    const size_t before = currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
    CHECK_DETAIL(before >= bytes, "recent-full accounting underflow: before=%zu remove=%zu", before, bytes);
}

void Report(size_t listRegions, size_t listBytes)
{
    const size_t in = enqueuedRegions.load(std::memory_order_relaxed);
    const size_t out = dequeuedRegions.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][recent-full-account] in=%zu out=%zu current_regions=%zu current_bytes=%zu "
         "peak_bytes=%zu list_regions=%zu list_bytes=%zu",
         in, out, in - out, currentBytes.load(std::memory_order_relaxed),
         peakBytes.load(std::memory_order_relaxed), listRegions, listBytes);
}
} // namespace RecentFullAccounting
}

namespace MapleRuntime {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
void RegionManager::SetAllocationStallTestHooks(AllocationStallTestHook beforeWave,
                                                AllocationStallTestHook requestGc,
                                                AllocationStallTestHook beforeWait)
{
    allocationStallBeforeWaveTestHook = std::move(beforeWave);
    allocationStallGcTestHook = std::move(requestGc);
    allocationStallBeforeWaitTestHook = std::move(beforeWait);
}

size_t RegionManager::PendingStalledAllocations() const { return allocationStallQueue.Pending(); }
size_t RegionManager::EnqueuedStalledAllocations() const { return allocationStallQueue.EnqueuedCount(); }
size_t RegionManager::DequeuedStalledAllocations() const { return allocationStallQueue.DequeuedCount(); }
size_t RegionManager::SatisfiedStalledAllocations() const { return allocationStallQueue.SatisfiedCount(); }
size_t RegionManager::FailedStalledAllocations() const { return allocationStallQueue.FailedCount(); }
#endif
} // namespace MapleRuntime
