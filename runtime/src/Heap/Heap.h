// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_H
#define MRT_HEAP_H

#include <cstdlib>
#include <functional>
#include <vector>
#include "Common/ColourEncoding.h"

#include "Barrier/Barrier.h"
#include "Base/ImmortalWrapper.h"
#include "Collector/Collector.h"
#include "Common/BaseObject.h"
#include "RuntimeConfig.h"

#include <unordered_set>
namespace MapleRuntime {
class Allocator;
class AllocBuffer;
class FinalizerProcessor;
class CollectorResources;


class Heap {
public:
    static Heap& GetHeap();
#ifdef MRT_TESTABLE_INTERNALS
    static size_t GetStaticRootCountForTesting();
#endif
    static Barrier& GetBarrier() { return **currentBarrierPtr; }
    virtual RememberedSet& GetRememberedSet() = 0;

    // concurrent gc uses barrier to access heap.
    static bool UseBarrier() { return *currentBarrierPtr != stwBarrierPtr; }

    virtual void Init(const HeapParam& vmHeapParam) = 0;
    virtual void Fini() = 0;
    virtual bool IsSurvivedObject(const BaseObject*) const = 0;
    bool IsGarbage(const BaseObject* obj) const { return !IsSurvivedObject(obj); }

    virtual bool IsGcStarted() const = 0;
    virtual void WaitForGCFinish() = 0;

    virtual bool IsGCEnabled() const = 0;
    virtual void EnableGC(bool val) = 0;

    virtual MAddress Allocate(size_t size, AllocType allocType) = 0;

    virtual Collector& GetCollector() = 0;
    virtual Allocator& GetAllocator() = 0;
    /* to avoid misunderstanding, variant types of heap size are defined as followed:
     * |------------------------------ max capacity ---------------------------------|
     * |------------------------------ current capacity ------------------------|
     * |------------------------------ committed size -----------------------|
     * |------------------------------ used size -------------------------|
     * |------------------------------ allocated size -------------|
     * |------------------------------ net size ------------|
     * so that inequality size <= capacity <= max capacity always holds.
     */
    virtual size_t GetMaxCapacity() const = 0;

    // or current capacity: a continuous address space to help heap management such as GC.
    virtual size_t GetCurrentCapacity() const = 0;

    // already used by allocator, including memory block cached for speeding up allocation.
    // we measure it in OS page granularity because physical memory is occupied by page.
    virtual size_t GetUsedPageSize() const = 0;

    // total memory allocated for each allocation request, including memory fragment for alignment or padding.
    virtual size_t GetAllocatedSize() const = 0;

    virtual MAddress GetStartAddress() const = 0;
    virtual MAddress GetSpaceEndAddress() const = 0;

    // Only reserved payload ranges are heap addresses. The outer address
    // envelope sizes offset tables, but its holes are never managed memory.
    static bool IsHeapAddress(MAddress addr)
    {
        for (const auto& range : heapReservations) {
            if (addr >= range.start && addr < range.end) {
                return true;
            }
        }
        return false;
    }

    static bool IsHeapAddress(const void* addr) { return IsHeapAddress(reinterpret_cast<MAddress>(addr)); }

    virtual void InstallBarrier(const GCPhase) = 0;

    virtual GCPhase GetGCPhase() const = 0;

    virtual void SetGCPhase(const GCPhase phase) = 0;

    virtual bool ForEachObj(const std::function<void(BaseObject*)>&, bool safe) const = 0;

    virtual void RegisterStaticRoots(Uptr, U32) = 0;

    virtual void UnregisterStaticRoots(Uptr, U32) = 0;

    virtual void VisitStaticRoots(const RootSlotVisitor& visitor) = 0;

    virtual U64 RegisterExportRoot(BaseObject*) = 0;
    virtual void VisitAllExportRoots(const RootVisitor& visitor) = 0;

    virtual BaseObject* GetExportObject(U64) = 0;
    virtual void RemoveExportObject(U64) = 0;

    virtual void SetExportObjActiveState(U64, bool) = 0;
    virtual bool CheckExportObjState(U64, BaseObject*) = 0;

    virtual ssize_t GetHeapPhysicalMemorySize() const = 0;

    virtual FinalizerProcessor& GetFinalizerProcessor() = 0;

    virtual CollectorResources& GetCollectorResources() = 0;

    virtual void RegisterAllocBuffer(AllocBuffer& buffer) = 0;

    virtual void RemoveAllocBuffer(AllocBuffer& buffer) = 0;

    virtual void CrossAccessBarrier(I64) = 0;

    virtual void StopGCWork() = 0;

    // Partial-array mark entries encode a 4K-shifted heap-relative offset
    // (zMark.cpp:177-186).  The codec owns the relative-alignment predicate;
    // heap creation still rejects an invalid production origin.
    static void CheckHeapStartAlignment(MAddress startAddr);

    static MAddress GetHeapStartAddress() { return heapStartAddr; }

#ifdef MRT_TESTABLE_INTERNALS
    // Test-only injection seam for the arbitrary-base codec arm. Production
    // writes remain confined to OnHeapCreated below.
    static void SetHeapStartForTesting(MAddress startAddr) { heapStartAddr = startAddr; }
#endif

    static void OnHeapCreated(MAddress startAddr)
    {
        CheckHeapStartAlignment(startAddr);
        heapStartAddr = startAddr;
        heapCurrentEnd = 0;
        heapReservations.clear();
    }

    static void OnHeapCreated(MAddress startAddr, const std::vector<HeapSlotAddressRange>& reservations)
    {
        OnHeapCreated(startAddr);
        for (const auto& range : reservations) {
            CHECK(range.end > range.start && IsRepresentableLow48Range(range.start, range.end - range.start));
        }
        heapReservations = reservations;
    }

    static void OnHeapExtended(MAddress newEnd)
    {
        CHECK(newEnd > heapStartAddr && IsRepresentableLow48Range(heapStartAddr, newEnd - heapStartAddr));
        if (heapReservations.empty()) {
            heapReservations.push_back({ heapStartAddr, newEnd });
        } else {
            heapReservations.back().end = newEnd;
        }
        heapCurrentEnd = newEnd;
    }

    virtual ~Heap() {}
    static Barrier** currentBarrierPtr; // record ptr for fast access
    static Barrier* stwBarrierPtr;      // record nonGC barrier
    static MAddress heapCurrentEnd;

private:
    static MAddress heapStartAddr;
    static std::vector<HeapSlotAddressRange> heapReservations;
};
} // namespace MapleRuntime
#endif // MRT_HEAP_MANAGER_H
