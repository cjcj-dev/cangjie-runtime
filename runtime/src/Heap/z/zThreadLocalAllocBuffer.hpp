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
    ZPage* GetRegion() const;
    size_t TLABSize() const { return tlab.end - tlab.start; }
    // zObjectAllocator.hpp per-thread current-page shape: staging is a
    // vector of page pointers, committed to RecentFull/RecentLarge roles.
    std::vector<ZPage*>& GetTlRawPointerRegions() { return tlRawPointerRegions; }
    std::vector<ZPage*>& GetTlLargeRawPointerRegions() { return tlLargeRawPointerRegions; }
    void FillTLAB(uintptr_t start, size_t size);
    void ClearRegion();

    size_t ComputeTLABSize(size_t objectSize, size_t maxSize) const;
    void AccumulateTLABStatistics(TLABStatistics& total, size_t used, size_t capacity);
    void ResizeTLAB(size_t capacity, double fallbackFraction, size_t maxSize);


    void CommitRawPointerRegions();

    void FlushRegion();
    void RetireTLAB(bool gcWaste);

private:

    // slow path
    MAddress TryAllocateOnce(size_t totalSize, AllocType allocType);
    MAddress AllocateImpl(size_t totalSize, AllocType allocType);
    MAddress AllocateRawPointerObject(size_t totalSize);

    // Temporary compiler ABI indirection, owned by cjcj-llvm#7.
    // The descriptor is not a ZPage: its bounds delimit a shared-page slice.
    struct TLAB {
        uintptr_t top = 0;
        uintptr_t end = 0;
        uintptr_t start = 0;
    };
    TLAB* tlabDescriptor = &tlab;
    TLAB tlab;
    static constexpr size_t MinTLABSize = 2 * 1024;
    uintptr_t AllocateInTLAB(size_t size);

    // HotSpot ThreadLocalAllocBuffer: thread-owned statistics survive refills
    // and reset only at a young-cycle boundary.
    TLABStatistics tlabStatistics;
    TLABAllocationAverage tlabAllocationFraction;
    std::atomic<size_t> desiredTLABSize{ MinTLABSize };
    std::atomic<size_t> tlabRefills{ 0 };

    // Allocation work is handed to marking as an atomic batch.
    mutable std::mutex handoffLock;

    // allocate objects which are exposed to runtime thus can not be moved.
    // allocation context is responsible to notify collector when these objects are safe to be collected.
    std::vector<ZPage*> tlRawPointerRegions;
    std::vector<ZPage*> tlLargeRawPointerRegions;
    mutable std::mutex y2yDirtyLock;
    std::unordered_set<BaseObject*> y2yDirtyHolders;
    std::unordered_set<MAddress> y2yDirtySlots;
};
} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_H
