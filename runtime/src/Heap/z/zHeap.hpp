// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_H
#define MRT_HEAP_H

#include "Heap/z/zServiceability.hpp"

#include <cstdlib>
#include <functional>
#include <vector>
#include "Common/ColourEncoding.h"

#include "Heap/z/zBarrier.hpp"
#include "Base/ImmortalWrapper.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Common/BaseObject.h"
#include "RuntimeConfig.h"

#include <unordered_set>
extern "C" {
extern uintptr_t g_cjHeapStart;
extern uintptr_t g_cjHeapEnd;
extern uintptr_t g_cjHeapRangeCount;
extern uintptr_t g_cjHeapRangeStart[];
extern uintptr_t g_cjHeapRangeEnd[];
}
namespace MapleRuntime {
class RegionInfo;
class ZPageTable;
class OopStorage;
enum class HeapDumpKind { NORMAL, OOM, IDE };
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
    static Barrier& GetBarrier() { return *barrierPtr; }
    virtual RememberedSet& GetRememberedSet() = 0;


    virtual void Init(const HeapParam& vmHeapParam) = 0;
    virtual void Fini() = 0;
    virtual bool IsSurvivedObject(const BaseObject*) const = 0;
    bool IsGarbage(const BaseObject* obj) const { return !IsSurvivedObject(obj); }

    virtual bool IsGcStarted() const = 0;

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
    virtual ZMemoryUsageInfo GetMemoryUsage() const = 0;

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

    static RegionInfo* page(MAddress addr);
    static bool is_in(MAddress addr);
    static bool is_young(MAddress addr);
    static bool is_old(MAddress addr);
    static ZPageTable& page_table();


    void DumpHeap(HeapDumpKind kind);

    virtual GCPhase GetGCPhase(GCCycleGeneration generation) const = 0;
    virtual void SetGCPhase(GCCycleGeneration generation, GCPhase phase) = 0;

    virtual bool ForEachObj(const std::function<void(BaseObject*)>&, bool safe) const = 0;

    virtual void RegisterStaticRoots(Uptr, U32) = 0;

    virtual void UnregisterStaticRoots(Uptr, U32) = 0;

    virtual void VisitStaticRoots(const NativeSlotVisitor& visitor) = 0;

    virtual U64 RegisterExportRoot(BaseObject*) = 0;
    virtual OopStorage& GetExportRootStorage() = 0;
    virtual void VisitAllExportRoots(const NativeSlotVisitor& visitor) = 0;

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

    static MAddress GetHeapStartAddress() { return heapStartAddr; }

    static void OnHeapCreated(MAddress startAddr)
    {
        heapStartAddr = startAddr;
        heapCurrentEnd = 0;
        heapReservations.clear();
        g_cjHeapStart = startAddr;
        g_cjHeapEnd = 0;
        PublishCompilerHeapRanges();
    }

    static void OnHeapCreated(MAddress startAddr, const std::vector<HeapSlotAddressRange>& reservations)
    {
        OnHeapCreated(startAddr);
        for (const auto& range : reservations) {
            CHECK(range.end > range.start && IsRepresentableLow48Range(range.start, range.end - range.start));
        }
        heapReservations = reservations;
        if (!reservations.empty()) {
            g_cjHeapStart = reservations.front().start;
            g_cjHeapEnd = reservations.back().end;
        }
        PublishCompilerHeapRanges();
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
        g_cjHeapEnd = newEnd;
        if (g_cjHeapStart == 0) {
            g_cjHeapStart = heapStartAddr;
        }
        PublishCompilerHeapRanges();
    }

    virtual ~Heap() {}
    static Barrier* barrierPtr;
    static MAddress heapCurrentEnd;

private:
    static void PublishCompilerHeapRanges()
    {
        constexpr unsigned kCap = kCjHeapRangeCap;
        CHECK_DETAIL(heapReservations.size() <= kCap,
                     "compiler heap range capacity exceeded: %zu > %u", heapReservations.size(), kCap);
        for (unsigned i = 0; i < kCap; ++i) {
            g_cjHeapRangeStart[i] = 0;
            g_cjHeapRangeEnd[i] = 0;
        }
        const unsigned n = static_cast<unsigned>(heapReservations.size());
        g_cjHeapRangeCount = n;
        for (unsigned i = 0; i < n; ++i) {
            g_cjHeapRangeStart[i] = heapReservations[i].start;
            g_cjHeapRangeEnd[i] = heapReservations[i].end;
        }
    }

    static MAddress heapStartAddr;
    static std::vector<HeapSlotAddressRange> heapReservations;
};
} // namespace MapleRuntime
#endif // MRT_HEAP_MANAGER_H
