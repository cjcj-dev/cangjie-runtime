// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_H
#define MRT_HEAP_H

#include "Heap/z/zServiceability.hpp"
#include "Heap/z/zTLABUsage.hpp"
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
class AllocBuffer;
class FinalizerProcessor;
struct ThreadLocalData;
struct ThreadGCData;
class ZRemembered;

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
    RegionManager& page_allocator();
    const RegionManager& page_allocator() const;
    uintptr_t alloc_tlab(size_t size);
    size_t tlab_used() const;
    size_t tlab_capacity() const;
    void reset_tlab_used();
    void account_alloc_page(ZPage* page);
    void account_undo_alloc_page(ZPage* page);
    size_t max_tlab_size() const { return ZObjectSizeLimitSmall; }
    size_t unsafe_max_tlab_alloc() const;
    void undo_alloc_object_for_relocation(MAddress addr, size_t size);
    ZObjectAllocator& object_allocator() { return _object_allocator; }
    ZCrossVM& cross_vm() { return _cross_vm; }
    const ZCrossVM& cross_vm() const { return _cross_vm; }
    void MarkObjectIfActive(BaseObject* object);
    Generation ObjectGeneration(BaseObject* object) const;
    bool OldActiveRemsetIsCurrent() const
    {
        return (*ZGeneration::old()).ActiveRemsetIsCurrent(
            (*ZGeneration::young()).Sequence());
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
                                  PageAge age = PageAge::eden, ZAllocationFlags flags = {});
    static ZPage* alloc_page(ZPage* page);
    static void free_page(ZPage* page);
    static size_t free_empty_pages(ZGenerationId id, const ZArray<ZPage*>* pages);


    void DumpHeap(HeapDumpKind kind);
    void object_iterate(ObjectClosure* object_cl, bool visit_weaks);
    void object_and_field_iterate_for_verify(ObjectClosure* object_cl, OopFieldClosure* field_cl, bool visit_weaks);







    ssize_t GetHeapPhysicalMemorySize() const;

    FinalizerProcessor& GetFinalizerProcessor();





    void StopGCWork();


private:
    static Heap* _heap;
    // zHeap.hpp:48-56: the heap directly owns the page allocator; its
    // mapped caches and backing resources outlive both generation members.
    RegionManager _page_allocator;
    ZPageTable _page_table;
    ZObjectAllocator _object_allocator;
    ZServiceability _serviceability;
    ZGenerationOld _old;
    ZGenerationYoung _young;
    ZTLABUsage _tlab_usage;
    // Cangjie foreign-cycle ownership has no Java/JNI counterpart.
    ZCrossVM _cross_vm;
    std::atomic<bool> isGCEnabled { true };
    bool _initialized { false };


};
} // namespace MapleRuntime

#include "Heap/z/zRelocationSet.inline.hpp"
#endif // MRT_HEAP_MANAGER_H
