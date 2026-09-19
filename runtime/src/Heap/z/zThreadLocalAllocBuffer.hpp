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

    // h3seed2: young→young write dirties the *holder object* (not the field slot).
    // Minor root enum merges these into the product work stack so FYS closure reaches
    // ArrayList/HashMap containers without recording every y2y field in remset.
    // Dedup per mutator: unique objects, not per-field writes (y2yN is millions).
    void PushY2yDirtyHolder(BaseObject* obj)
    {
        if (obj != nullptr) {
            std::lock_guard<std::mutex> lock(y2yDirtyLock);
            y2yDirtyHolders.insert(obj);
        }
    }

    template<class WorkStack>
    inline void MergeY2yDirtyHolders(WorkStack& workStack)
    {
        decltype(y2yDirtyHolders) pending;
        {
            std::lock_guard<std::mutex> lock(y2yDirtyLock);
#if defined(MRT_TESTABLE_INTERNALS)
            if (y2yDirtyHolderMergeHook != nullptr) {
                y2yDirtyHolderMergeHook(y2yDirtyHolderMergeHookContext);
            }
#endif
            pending.swap(y2yDirtyHolders);
        }
        for (BaseObject* obj : pending) {
            PushDirtyHolder(workStack, obj);
        }
    }

    // Mutator-side publication of a dirty holder (ZMark::mark_object with
    // gc_thread = false, zMark.inline.hpp:82): mark + inc_live + follow.
    static void PushDirtyHolder(MarkStack<MarkStackEntry>& workStack, BaseObject* obj)
    {
        workStack.push_back(MarkStackEntry(untype(ZAddress::offset(from_object(obj))), true, true, true, false));
    }

    template<class Container>
    static void PushDirtyHolder(Container& container, BaseObject* obj)
    {
        container.push_back(obj);
    }

    size_t Y2yDirtyHolderCount() const
    {
        std::lock_guard<std::mutex> lock(y2yDirtyLock);
        return y2yDirtyHolders.size();
    }

    // A compiler barrier may know that the destination is a heap slot without
    // carrying a usable holder object.  Young slots are deliberately absent
    // from the remembered set, so retain the exact slot as mark work instead.
    // The young-mark owner consumes this list before relocation starts and
    // resolves the current target from the product HeapSlot.
    void PushY2yDirtySlot(MAddress slot)
    {
        if (slot != 0) {
            std::lock_guard<std::mutex> lock(y2yDirtyLock);
            y2yDirtySlots.insert(slot);
        }
    }

    template<class Visitor>
    inline void MergeY2yDirtySlots(Visitor&& visitor)
    {
        decltype(y2yDirtySlots) pending;
        {
            std::lock_guard<std::mutex> lock(y2yDirtyLock);
            pending.swap(y2yDirtySlots);
        }
        for (MAddress slot : pending) {
            visitor(slot);
        }
    }

    size_t Y2yDirtySlotCount() const
    {
        std::lock_guard<std::mutex> lock(y2yDirtyLock);
        return y2yDirtySlots.size();
    }

#if defined(MRT_TESTABLE_INTERNALS)
    using Y2yDirtyHolderMergeHook = void (*)(void*);
    void SetY2yDirtyHolderMergeHookForTest(Y2yDirtyHolderMergeHook hook, void* context);
#endif

#if defined(MRT_GC_UNIT_TESTS)
    // gc_unit only.  Fires at the one instant the unsynchronised handoff has and
    // a swap handoff does not: the consumer has determined the batch it will
    // deliver, and has not yet retired that batch from the mutator-owned
    // container.  A publication that lands in this interval is dropped by the
    // following clear() and never reaches any batch.
    using HandoffHook = void (*)(void*);
#endif

    void FlushRegion();
    void RetireTLAB(bool gcWaste);

private:
#if defined(MRT_GC_UNIT_TESTS)
    static void FireHandoffHook(HandoffHook hook, void* context);
#endif

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
    // h3seed2: mutator-local young→young dirty holders (see PushY2yDirtyHolder)
    mutable std::mutex y2yDirtyLock;
    std::unordered_set<BaseObject*> y2yDirtyHolders;
    // Holder-independent peer for compiler ABI calls that carry only a heap slot.
    std::unordered_set<MAddress> y2yDirtySlots;
#if defined(MRT_TESTABLE_INTERNALS)
    Y2yDirtyHolderMergeHook y2yDirtyHolderMergeHook{ nullptr };
    void* y2yDirtyHolderMergeHookContext{ nullptr };
#endif
#if defined(MRT_GC_UNIT_TESTS)
    // Last, so tlRegion keeps offset 0 (RegionSpace.cpp:255 static_assert).
#endif
};
} // namespace MapleRuntime
#include "Heap/Allocator/AllocBuffer.h"
#endif // MRT_ALLOC_BUFFER_H
