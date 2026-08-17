// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOC_BUFFER_H
#define MRT_ALLOC_BUFFER_H

#include <functional>
#include <unordered_set>

#include "Common/MarkWorkStack.h"
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

    // record stack roots in allocBuffer so that mutator can concurrently enumerate roots without lock.
    void PushRoot(BaseObject* root) { stackRoots.emplace_back(root); }

    // move the stack roots to other container so that other threads can visit them.
    template<class WorkStack>
    inline void MergeRoots(WorkStack& workStack)
    {
        if (stackRoots.empty()) {
            return;
        }
        for (BaseObject* obj : stackRoots) {
            workStack.push_back(obj);
        }
        stackRoots.clear();
    }

    // youngconc: TRACE-window allocate-black greys (mutator-only push; GC merges at STW2).
    // Paint alone makes MarkObject claim skip TraceYoungClosure → never reachableVec/fields.
    void PushYoungAllocBlack(BaseObject* obj) { youngAllocBlack.emplace_back(obj); }

    template<class WorkStack>
    inline void MergeYoungAllocBlack(WorkStack& workStack)
    {
        if (youngAllocBlack.empty()) {
            return;
        }
        for (BaseObject* obj : youngAllocBlack) {
            workStack.push_back(obj);
        }
        youngAllocBlack.clear();
    }

    // h3seed2: young→young write dirties the *holder object* (not the field slot).
    // Minor root enum merges these into the product work stack so FYS closure reaches
    // ArrayList/HashMap containers without recording every y2y field in remset.
    // Dedup per mutator: unique objects, not per-field writes (y2yN is millions).
    void PushY2yDirtyHolder(BaseObject* obj)
    {
        if (obj != nullptr) {
            y2yDirtyHolders.insert(obj);
        }
    }

    template<class WorkStack>
    inline void MergeY2yDirtyHolders(WorkStack& workStack)
    {
        if (y2yDirtyHolders.empty()) {
            return;
        }
        for (BaseObject* obj : y2yDirtyHolders) {
            workStack.push_back(obj);
        }
        y2yDirtyHolders.clear();
    }

    size_t Y2yDirtyHolderCount() const { return y2yDirtyHolders.size(); }

    void FlushRegion();

private:
    // slow path
    MAddress TryAllocateOnce(size_t totalSize, AllocType allocType);
    MAddress AllocateImpl(size_t totalSize, AllocType allocType);
    MAddress AllocateRawPointerObject(size_t totalSize);

    // tlRegion in AllocBuffer is a shortcut for fast allocation.
    // we should handle failure in RegionManager
    RegionInfo* tlRegion = RegionInfo::NullRegion();

    std::atomic<RegionInfo*> preparedRegion = { nullptr };
    // allocate objects which are exposed to runtime thus can not be moved.
    // allocation context is responsible to notify collector when these objects are safe to be collected.
    RegionList tlRawPointerRegions;
    RegionList tlLargeRawPointerRegions;
    // Record stack roots in concurrent enum phase, waiting for GC to merge these roots
    std::list<BaseObject*> stackRoots;
    // youngconc allocate-black greys (see PushYoungAllocBlack)
    std::list<BaseObject*> youngAllocBlack;
    // h3seed2: mutator-local young→young dirty holders (see PushY2yDirtyHolder)
    std::unordered_set<BaseObject*> y2yDirtyHolders;
};
} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_H
