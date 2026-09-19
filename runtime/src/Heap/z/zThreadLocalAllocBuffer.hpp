// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOC_BUFFER_H
#define MRT_ALLOC_BUFFER_H

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_set>

#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zMarkStackEntry.hpp"

#include "Heap/z/zTLABUsage.hpp"
#include "Heap/z/zPageFwd.hpp"
#include "Base/Globals.h"
namespace MapleRuntime {
// HotSpot gcUtil.cpp:29-56, TLABAllocationWeight=35. Startup samples
// use 1/n until the configured weight dominates.
class AllocBuffer {
public:
    AllocBuffer() = default;
    ~AllocBuffer();
    void Init();
    void Fini();
    static AllocBuffer* GetOrCreateAllocBuffer();
    static AllocBuffer* GetAllocBuffer();

    MAddress Allocate(size_t size, AllocType allocType);
    ZPage* GetRegion() { return tlRegion; }
    // zObjectAllocator.hpp per-thread current-page shape: staging is a
    // vector of page pointers, committed to RecentFull/RecentLarge roles.
    std::vector<ZPage*>& GetTlRawPointerRegions() { return tlRawPointerRegions; }
    std::vector<ZPage*>& GetTlLargeRawPointerRegions() { return tlLargeRawPointerRegions; }
    ZPage* GetPreparedRegion() { return preparedRegion.load(std::memory_order_relaxed); }
    void SetRegion(ZPage* newRegion);
    void ClearRegion();

    size_t ComputeTLABSize(size_t objectSize, size_t maxSize) const;
    void AccumulateTLABStatistics(TLABStatistics& total, size_t used, size_t capacity);
    void ResizeTLAB(size_t capacity, double fallbackFraction, size_t maxSize);

    bool SetPreparedRegion(ZPage* newPreparedRegion)
    {
        ZPage* expect = nullptr;
        return preparedRegion.compare_exchange_strong(expect, newPreparedRegion, std::memory_order_release);
    }
    void CommitRawPointerRegions();

    void FlushRegion();
    void RetireTLAB(bool gcWaste);

private:

    // slow path
    MAddress TryAllocateOnce(size_t totalSize, AllocType allocType);
    MAddress AllocateImpl(size_t totalSize, AllocType allocType);
    MAddress AllocateRawPointerObject(size_t totalSize);

    // tlRegion in AllocBuffer is a shortcut for fast allocation.
    // we should handle failure in RegionManager
    ZPage* tlRegion = nullptr;

    // HotSpot ThreadLocalAllocBuffer: thread-owned statistics survive refills
    // and reset only at a young-cycle boundary. Async refill reads atomics only.
    TLABStatistics tlabStatistics;
    TLABAllocationAverage tlabAllocationFraction;
    std::atomic<size_t> desiredTLABSize{ MRT_PAGE_SIZE };
    std::atomic<size_t> tlabRefills{ 0 };

    // Allocation work is handed to marking as an atomic batch.
    mutable std::mutex handoffLock;

    std::atomic<ZPage*> preparedRegion = { nullptr };
    // allocate objects which are exposed to runtime thus can not be moved.
    // allocation context is responsible to notify collector when these objects are safe to be collected.
    std::vector<ZPage*> tlRawPointerRegions;
    std::vector<ZPage*> tlLargeRawPointerRegions;

};
} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_H
