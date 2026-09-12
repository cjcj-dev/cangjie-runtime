// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOC_BUFFER_H
#define MRT_ALLOC_BUFFER_H

#include <functional>
#include <mutex>
#include <unordered_set>

#include "Common/MarkWorkStack.h"
#include "Heap/Collector/MarkStackEntry.h"
#include "RegionList.h"

namespace MapleRuntime {
// thread-local data structure
class AllocBuffer {
public:
    AllocBuffer() : tlRawPointerRegions("thread-local raw-pointer regions"),
                    tlLargeRawPointerRegions("thread-local large raw-pointer regions") {}
    ~AllocBuffer();
    void Init();
    void Fini();
    static AllocBuffer* GetOrCreateAllocBuffer();
    static AllocBuffer* GetAllocBuffer();

    MAddress Allocate(size_t size, AllocType allocType);
    RegionInfo* GetRegion() { return tlRegion; }
    RegionList& GetTlRawPointerRegions() { return tlRawPointerRegions; }
    RegionList& GetTlLargeRawPointerRegions() { return tlLargeRawPointerRegions; }
    RegionInfo* GetPreparedRegion() { return preparedRegion.load(std::memory_order_relaxed); }
    void SetRegion(RegionInfo* newRegion) { tlRegion = newRegion; }
    inline void ClearRegion()
    {
        if (tlRegion == RegionInfo::NullRegion()) {
            return;
        }
        DLOG(REGION, "alloc buffer clear tlRegion %p@[0x%zx, 0x%zx)", tlRegion, tlRegion->GetRegionStart(),
             tlRegion->GetRegionEnd());
        tlRegion = RegionInfo::NullRegion();
    }

    bool SetPreparedRegion(RegionInfo* newPreparedRegion)
    {
        RegionInfo* expect = nullptr;
        return preparedRegion.compare_exchange_strong(expect, newPreparedRegion, std::memory_order_release);
    }
    void CommitRawPointerRegions();

    // youngconc: TRACE-window allocate-black greys (mutator-only push; GC merges at STW2).
    // Paint alone makes MarkObject claim skip TraceYoungClosure → never reachableVec/fields.
    void PushYoungAllocBlack(BaseObject* obj)
    {
        std::lock_guard<std::mutex> lock(handoffLock);
        youngAllocBlack.emplace_back(obj);
    }
    size_t YoungAllocBlackCount() const
    {
        std::lock_guard<std::mutex> lock(handoffLock);
        return youngAllocBlack.size();
    }

    template<class WorkStack>
    inline void MergeYoungAllocBlack(WorkStack& workStack)
    {
        std::list<BaseObject*> pending;
        {
            std::lock_guard<std::mutex> lock(handoffLock);
            pending.swap(youngAllocBlack);
        }
#if defined(MRT_GC_UNIT_TESTS)
        FireHandoffHook(youngAllocBlackHandoffHook, youngAllocBlackHandoffHookContext);
#endif
        for (BaseObject* obj : pending) {
            workStack.push_back(obj);
        }
    }

    // Already-painted allocate-black work must keep the Follow bit. A plain
    // MarkAndFollow push would skip children once the mark bit is claimed.
    template<class WorkStack>
    inline void MergeYoungAllocBlackFollow(WorkStack& workStack)
    {
        std::list<BaseObject*> pending;
        {
            std::lock_guard<std::mutex> lock(handoffLock);
            pending.swap(youngAllocBlack);
        }
#if defined(MRT_GC_UNIT_TESTS)
        FireHandoffHook(youngAllocBlackHandoffHook, youngAllocBlackHandoffHookContext);
#endif
        for (BaseObject* obj : pending) {
            workStack.push_back(MarkStackEntry::FollowOnly(obj));
        }
    }

    // Observe-only: STW2 current-face audit (Stw2CurrentAudit) classifies without
    // consuming the grey-list MergeYoungAllocBlack still owns.
    template<class WorkStack>
    inline void PeekYoungAllocBlack(WorkStack& workStack) const
    {
        std::lock_guard<std::mutex> lock(handoffLock);
        for (BaseObject* obj : youngAllocBlack) {
            workStack.push_back(obj);
        }
    }

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
            workStack.push_back(obj);
        }
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
    void SetY2yDirtyHolderMergeHookForTest(Y2yDirtyHolderMergeHook hook, void* context)
    {
        std::lock_guard<std::mutex> lock(y2yDirtyLock);
        y2yDirtyHolderMergeHook = hook;
        y2yDirtyHolderMergeHookContext = context;
    }
#endif

#if defined(MRT_GC_UNIT_TESTS)
    // gc_unit only.  Fires at the one instant the unsynchronised handoff has and
    // a swap handoff does not: the consumer has determined the batch it will
    // deliver, and has not yet retired that batch from the mutator-owned
    // container.  A publication that lands in this interval is dropped by the
    // following clear() and never reaches any batch.
    using HandoffHook = void (*)(void*);
    void SetYoungAllocBlackHandoffHookForTest(HandoffHook hook, void* context)
    {
        youngAllocBlackHandoffHook = hook;
        youngAllocBlackHandoffHookContext = context;
    }
#endif

    void FlushRegion();


private:
#if defined(MRT_GC_UNIT_TESTS)
    static void FireHandoffHook(HandoffHook hook, void* context)
    {
        if (hook != nullptr) {
            hook(context);
        }
    }
#endif

    // slow path
    MAddress TryAllocateOnce(size_t totalSize, AllocType allocType);
    MAddress AllocateImpl(size_t totalSize, AllocType allocType);
    MAddress AllocateRawPointerObject(size_t totalSize);

    // tlRegion in AllocBuffer is a shortcut for fast allocation.
    // we should handle failure in RegionManager
    RegionInfo* tlRegion = RegionInfo::NullRegion();

    // Allocation work is handed to marking as an atomic batch.
    mutable std::mutex handoffLock;

    std::atomic<RegionInfo*> preparedRegion = { nullptr };
    // allocate objects which are exposed to runtime thus can not be moved.
    // allocation context is responsible to notify collector when these objects are safe to be collected.
    RegionList tlRawPointerRegions;
    RegionList tlLargeRawPointerRegions;
    // youngconc allocate-black greys (see PushYoungAllocBlack)
    std::list<BaseObject*> youngAllocBlack;
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
    HandoffHook youngAllocBlackHandoffHook{ nullptr };
    void* youngAllocBlackHandoffHookContext{ nullptr };
#endif
};
} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_H
