// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_H
#define MRT_HEAP_H

#include "Heap/z/zServiceability.hpp"
#include "Heap/z/zCrossVM.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>
#include "Common/ColourEncoding.h"

#include "Heap/z/zBarrier.hpp"
#include "Base/ImmortalWrapper.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include <memory>
#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zPageType.hpp"
#include "Heap/z/zPageFwd.hpp"
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"
#include "RuntimeConfig.h"

#include <atomic>
#include <unordered_set>
extern "C" {
extern uintptr_t g_cjHeapStart;
extern uintptr_t g_cjHeapEnd;
extern uintptr_t g_cjHeapRangeCount;
extern uintptr_t g_cjHeapRangeStart[];
extern uintptr_t g_cjHeapRangeEnd[];
}
#include "Heap/z/zAllocationFlags.hpp"

namespace MapleRuntime {
template<typename T> class ZArray;
class ZPageTable;
class OopStorage;
class ObjectClosure;
class OopFieldClosure;
enum class HeapDumpKind { NORMAL, OOM, IDE };
class RegionSpace;
class AllocBuffer;
class FinalizerProcessor;
struct ThreadLocalData;
struct ThreadGCData;
class ZRemembered;
class ExportRootTable;
class StaticRootTable;

class Heap {
    friend class ZCollectedHeap;
public:
    static Heap& GetHeap();
    static Heap* heap() { return _heap; }
    Heap(const HeapParam& param, double garbageThreshold);
    ~Heap();
    ZRemembered& remembered();
    void Init();
    void Fini();
    bool IsSurvivedObject(const BaseObject*) const;
    bool IsGarbage(const BaseObject* obj) const { return !IsSurvivedObject(obj); }

    bool IsGcStarted() const;

    bool IsGCEnabled() const;
    void EnableGC(bool val);

    MAddress Allocate(size_t size, AllocType allocType);

    void RequestGC(GCReason reason);
    void ResolveCycleRef();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    void DumpRoots(LogType logType);
    void DumpHeap(const CString& tag);
    void DumpBeforeGC();
    void DumpAfterGC();
#endif
    RegionSpace& GetAllocator();
    RegionManager& page_allocator();
    const RegionManager& page_allocator() const;
    uintptr_t alloc_tlab(size_t size);
    size_t max_tlab_size() const { return ZObjectSizeLimitSmall; }
    size_t unsafe_max_tlab_alloc() const;
    void undo_alloc_object_for_relocation(MAddress addr, size_t size);
    ZObjectAllocator& object_allocator() { return _object_allocator; }
    ZCrossVM& cross_vm() { return _cross_vm; }
    const ZCrossVM& cross_vm() const { return _cross_vm; }
    void MarkObjectIfActive(BaseObject* object);
    void MarkYoungObjectIfActive(BaseObject* object);
    void MarkNewObject(BaseObject* object);
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId generation);
    BaseObject* make_load_good(RefField<>& ref);
    Generation ObjectGeneration(BaseObject* object) const;
    GCCycleSnapshot GetCycleSnapshot(ZGenerationId generation) const
    {
        return GetZGeneration(generation).Snapshot();
    }
    bool OldActiveRemsetIsCurrent() const
    {
        return GetZGeneration(ZGenerationId::old).ActiveRemsetIsCurrent(
            GetZGeneration(ZGenerationId::young).Sequence());
    }
    void PublishGenerationPhase(ZGenerationId generation, ZGenerationPhase value);
    void mark_flush(ThreadGCData& data);
    bool FlushGCDataMarkProducers(ThreadGCData& data);
    bool FlushThreadMarkProducers(ThreadLocalData* tls);
    bool IsGhostFromObject(BaseObject* obj) const;
    ZGenerationYoung& young() { return *ZGeneration::young(); }
    const ZGenerationYoung& young() const { return *ZGeneration::young(); }
    ZGenerationOld& old() { return *ZGeneration::old(); }
    const ZGenerationOld& old() const { return *ZGeneration::old(); }
    // zGeneration.cpp:600,637 (ZCollectedHeap::increment_total_collections)
    uint32_t total_collections() const { return _total_collections.load(std::memory_order_acquire); }
    void increment_total_collections() { _total_collections.fetch_add(1, std::memory_order_release); }

    ZGeneration& GetZGeneration(ZGenerationId generation)
    {
        if (generation == ZGenerationId::young) {
            return *ZGeneration::young();
        }
        return *ZGeneration::old();
    }
    const ZGeneration& GetZGeneration(ZGenerationId generation) const
    {
        if (generation == ZGenerationId::young) {
            return *ZGeneration::young();
        }
        return *ZGeneration::old();
    }
    ZGeneration& GetZGeneration(Generation generation)
    {
        return GetZGeneration(generation == Generation::Young ? ZGenerationId::young : ZGenerationId::old);
    }
    const ZGeneration& GetZGeneration(Generation generation) const
    {
        return GetZGeneration(generation == Generation::Young ? ZGenerationId::young : ZGenerationId::old);
    }
    /* to avoid misunderstanding, variant types of heap size are defined as followed:
     * |------------------------------ max capacity ---------------------------------|
     * |------------------------------ current capacity ------------------------|
     * |------------------------------ committed size -----------------------|
     * |------------------------------ used size -------------------------|
     * |------------------------------ allocated size -------------|
     * |------------------------------ net size ------------|
     * so that inequality size <= capacity <= max capacity always holds.
     */
    size_t GetMaxCapacity() const;
    size_t soft_max_capacity() const;
    ZMemoryUsageInfo GetMemoryUsage() const;

    // or current capacity: a continuous address space to help heap management such as GC.
    size_t GetCurrentCapacity() const;

    // already used by allocator, including memory block cached for speeding up allocation.
    // we measure it in OS page granularity because physical memory is occupied by page.
    size_t GetUsedPageSize() const;

    // total memory allocated for each allocation request, including memory fragment for alignment or padding.
    size_t GetAllocatedSize() const;

    MAddress GetStartAddress() const;
    MAddress GetSpaceEndAddress() const;

    // Only reserved payload ranges are heap addresses. The outer address
    // envelope sizes offset tables, but its holes are never managed memory.
    static bool IsHeapAddress(MAddress addr) { return is_heap_address(addr); }

    static bool IsHeapAddress(const void* addr) { return is_heap_address(addr); }

    static ZPage* page(MAddress addr);
    static bool is_in(MAddress addr);
    static bool is_young(MAddress addr);
    static bool is_old(MAddress addr);
    static ZPageTable& page_table();
    static ZPage* alloc_page(size_t num, ZPageType role, bool expectPhysicalMem = false,
                                  bool allowSaferegion = true,
                                  PageAge age = PageAge::eden, ZAllocationFlags flags = {});
    static ZPage* alloc_page(ZPage* page);
    static void free_page(ZPage* page);
    static size_t free_empty_pages(ZGenerationId id, const ZArray<ZPage*>* pages);


    void DumpHeap(HeapDumpKind kind);
    void object_iterate(ObjectClosure* object_cl, bool visit_weaks);
    void object_and_field_iterate_for_verify(ObjectClosure* object_cl, OopFieldClosure* field_cl, bool visit_weaks);

    void RegisterStaticRoots(Uptr, U32);

    void UnregisterStaticRoots(Uptr, U32);

    void VisitStaticRoots(const NativeSlotVisitor& visitor);

    U64 RegisterExportRoot(BaseObject*);
    OopStorage& GetExportRootStorage();
    void VisitAllExportRoots(const NativeSlotVisitor& visitor);

    BaseObject* GetExportObject(U64);
    void RemoveExportObject(U64);

    void SetExportObjActiveState(U64, bool);
    bool CheckExportObjState(U64, BaseObject*);

    ssize_t GetHeapPhysicalMemorySize() const;

    FinalizerProcessor& GetFinalizerProcessor();




    void CrossAccessBarrier(I64);

    void StopGCWork();

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

    static MAddress heapCurrentEnd;

private:
    static Heap* _heap;
    std::atomic<uint32_t> _total_collections{0};
    // zHeap.hpp:48-56: the heap directly owns the page allocator; its
    // mapped caches and backing resources outlive both generation members.
    RegionManager _page_allocator;
    // Object/TLAB adapter remains pending P16; it owns no page allocator.
    std::unique_ptr<RegionSpace> _allocation_adapter;
    ZPageTable _page_table;
    ZObjectAllocator _object_allocator;
    ZServiceability _serviceability;
    ZGenerationOld _old;
    ZGenerationYoung _young;
    // Cangjie foreign-cycle ownership has no Java/JNI counterpart.
    ZCrossVM _cross_vm;
    ExportRootTable* exportRootsTable { nullptr };
    StaticRootTable* staticRootTable { nullptr };
    std::atomic<bool> isGCEnabled { true };
    bool _initialized { false };

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

#include "Heap/z/zRelocationSet.inline.hpp"
#endif // MRT_HEAP_MANAGER_H
