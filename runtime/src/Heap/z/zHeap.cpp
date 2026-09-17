// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zArray.hpp"

#include "Heap/WCollector/WCollector.h"
#include "Heap/z/zArguments.hpp"
#include "Heap/z/zDriver.hpp"
#include "Interpreter/Options.h"
#include "Interpreter/InterpreterSpecific.h"
#include "Mutator/MutatorManager.h"
#if defined(_WIN64)
#include <windows.h>
#include <psapi.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.hpp"
#include "Heap/Allocator/RegionList.h"

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
#include "Heap/z/zInitialize.hpp"
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
MAddress Heap::heapStartAddr = 0;
MAddress Heap::heapCurrentEnd = 0;
std::vector<HeapSlotAddressRange> Heap::heapReservations;

class HeapImpl : public Heap {
public:
    HeapImpl()
        : theSpace(Allocator::NewAllocator()), collectorResources(collectorImpl),
          collectorImpl(*theSpace, collectorResources)
    {
        RunType::InitRunTypeMap();
    }

    ~HeapImpl() override = default;
    void Init(const HeapParam& vmHeapParam) override;
    void Fini() override;

    bool IsSurvivedObject(const BaseObject* obj) const override
    {
        // ZPage::is_object_live (zPage.inline.hpp:254-256).
        return Heap::page(reinterpret_cast<MAddress>(obj))->is_object_live(from_object(obj));
    }

    bool IsGcStarted() const override { return collectorResources.IsGcStarted(); }


    bool IsGCEnabled() const override { return isGCEnabled.load(); }

    void EnableGC(bool val) override { return isGCEnabled.store(val); }

    MAddress Allocate(size_t size, AllocType allocType) override;

    Collector& GetCollector() override;
    Allocator& GetAllocator() override;

    size_t GetMaxCapacity() const override;
    ZMemoryUsageInfo GetMemoryUsage() const override;
    size_t GetCurrentCapacity() const override;
    size_t GetUsedPageSize() const override;
    size_t GetAllocatedSize() const override;
    MAddress GetStartAddress() const override;
    MAddress GetSpaceEndAddress() const override;
    void RegisterStaticRoots(Uptr addr, U32) override;
    void UnregisterStaticRoots(Uptr addr, U32) override;
    void VisitStaticRoots(const NativeSlotVisitor& visitor) override;

    bool ForEachObj(const std::function<void(BaseObject*)>&, bool) const override;
    ssize_t GetHeapPhysicalMemorySize() const override;

    FinalizerProcessor& GetFinalizerProcessor() override;
    CollectorResources& GetCollectorResources() override;
    void RegisterAllocBuffer(AllocBuffer& buffer) override;
    void RemoveAllocBuffer(AllocBuffer& buffer) override;
    U64 RegisterExportRoot(BaseObject* obj) override;
    OopStorage& GetExportRootStorage() override { return exportRootsTable.RootStorage(); }
    void VisitAllExportRoots(const NativeSlotVisitor& visitor) override;
    BaseObject* GetExportObject(U64 id) override;
    void RemoveExportObject(U64 id) override;
    void StopGCWork() override;
    void CrossAccessBarrier(I64 handle) override;
    void SetExportObjActiveState(U64 id, bool state) override;
    bool CheckExportObjState(U64 id, BaseObject* exportObj) override;

    class ScopedFileHandler {
    public:
        ScopedFileHandler(const char* fileName, const char* mode) { file = fopen(fileName, mode); }
        ~ScopedFileHandler()
        {
            if (file != nullptr) {
                fclose(file);
            }
        }
        FILE* GetFile() const { return file; }

    private:
        FILE* file = nullptr;
    };

private:
    // allocator is actually a subspace in heap
    Allocator* theSpace;

    CollectorResources collectorResources;

    // collector is closely related to barrier. but we do not put barrier inside collector because even without
    // collector (i.e. no-gc), allocator and barrier (interface to access heap) is still needed.
    WCollector collectorImpl;

    ExportRootTable exportRootsTable;

    // manage gc roots entry
    StaticRootTable staticRootTable;

    std::atomic<bool> isGCEnabled = { true };
}; // end class HeapImpl

static ImmortalWrapper<HeapImpl> g_heapInstance;



MAddress HeapImpl::Allocate(size_t size, AllocType allocType) { return theSpace->Allocate(size, allocType); }

bool HeapImpl::ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const
{
    return theSpace->ForEachObj(visitor, safe);
}

void HeapImpl::Init(const HeapParam& param)
{
    ZArguments::initialize();
    ZHeuristics::set_max_heap_size(param.heapSize * 1024);
    ZInitialize::initialize();
    theSpace->Init(param);
    Heap::GetHeap().EnableGC(ZArguments::gc_enabled());
    collectorImpl.Init();
    {
        const auto& heapMap = ZPageTable::heap_table().map();
        const size_t heapSpan = heapMap.size() * heapMap.granule();
        collectorImpl.GetZGeneration(ZGenerationId::young).forwarding_table().initialize(
            heapSpan, heapMap.base(), heapMap.granule());
        collectorImpl.GetZGeneration(ZGenerationId::old).forwarding_table().initialize(
            heapSpan, heapMap.base(), heapMap.granule());
    }
    collectorImpl.GetZGeneration(ZGenerationId::young).remembered()->bind(
        &ZPageTable::heap_table(),
        &collectorImpl.GetZGeneration(ZGenerationId::old).forwarding_table(),
        &static_cast<RegionSpace*>(theSpace)->GetRegionManager());
    if (collectorImpl.GetZGeneration(ZGenerationId::young).Workers() == nullptr) {
        collectorImpl.GetZGeneration(ZGenerationId::young).InitializeWorkers(1);
    }
    if (collectorImpl.GetZGeneration(ZGenerationId::old).Workers() == nullptr) {
        collectorImpl.GetZGeneration(ZGenerationId::old).InitializeWorkers(1);
    }
    collectorResources.Init();
}

void HeapImpl::Fini()
{
    collectorResources.Fini();
    collectorImpl.GetZGeneration(ZGenerationId::young).StopWorkers();
    collectorImpl.GetZGeneration(ZGenerationId::old).StopWorkers();
    collectorImpl.Fini();
    if (theSpace != nullptr) {
        delete theSpace;
        theSpace = nullptr;
    }
}

Collector& HeapImpl::GetCollector() { return collectorResources.ActiveCollector(); }

Allocator& HeapImpl::GetAllocator() { return *theSpace; }

size_t HeapImpl::GetMaxCapacity() const { return theSpace->GetMaxCapacity(); }

ZMemoryUsageInfo HeapImpl::GetMemoryUsage() const
{
    return static_cast<RegionSpace*>(theSpace)->GetMemoryUsage();
}


size_t HeapImpl::GetCurrentCapacity() const { return theSpace->GetCurrentCapacity(); }

size_t HeapImpl::GetUsedPageSize() const { return theSpace->GetUsedPageSize(); }

size_t HeapImpl::GetAllocatedSize() const { return theSpace->AllocatedBytes(); }

MAddress HeapImpl::GetStartAddress() const { return theSpace->GetSpaceStartAddress(); }

MAddress HeapImpl::GetSpaceEndAddress() const { return theSpace->GetSpaceEndAddress(); }

Heap& Heap::GetHeap() { return *g_heapInstance; }

ZRemembered& Heap::remembered()
{
    return *GetCollector().GetZGeneration(ZGenerationId::young).remembered();
}

void HeapImpl::RegisterStaticRoots(Uptr addr, U32 size)
{
    staticRootTable.RegisterRoots(reinterpret_cast<StaticRootTable::StaticRootArray*>(addr), size);
}

void HeapImpl::UnregisterStaticRoots(Uptr addr, U32 size)
{
    staticRootTable.UnregisterRoots(reinterpret_cast<StaticRootTable::StaticRootArray*>(addr), size);
}

void HeapImpl::VisitStaticRoots(const NativeSlotVisitor& visitor)
{
    staticRootTable.VisitRoots(visitor);
#ifdef INTERPRETER_ENABLED
    VisitInterpreterGlobalRoots(&visitor);
#endif
}

#if defined(_WIN64)
ssize_t HeapImpl::GetHeapPhysicalMemorySize() const
{
    PROCESS_MEMORY_COUNTERS memCounter;
    HANDLE hProcess = GetCurrentProcess();
    if (GetProcessMemoryInfo(hProcess, &memCounter, sizeof(memCounter))) {
        size_t physicalMemorySize = memCounter.WorkingSetSize;
        if (physicalMemorySize > std::numeric_limits<ssize_t>::max()) {
            LOG(RTLOG_ERROR, "PhysicalMemorySize is too large");
            CloseHandle(hProcess);
            return -2; // -2: Return value of exception.
        }
        CloseHandle(hProcess);
        return static_cast<ssize_t>(physicalMemorySize);
    } else {
        LOG(RTLOG_ERROR, "GetHeapPhysicalMemorySize fail");
        CloseHandle(hProcess);
        return -2; // -2: Return value of exception.
    }
    return 0;
}

#elif defined(__APPLE__)
ssize_t HeapImpl::GetHeapPhysicalMemorySize() const
{
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count) != KERN_SUCCESS)
        return -2;  // -2: Return value of exception.
    return t_info.resident_size;
}

#else
ssize_t HeapImpl::GetHeapPhysicalMemorySize() const
{
    CString smapsFile = CString("/proc/") + CString(MapleRuntime::GetPid()) + "/smaps";
    ScopedFileHandler fileHandler(smapsFile.Str(), "r");
    FILE* file = fileHandler.GetFile();
    if (file == nullptr) {
        LOG(RTLOG_ERROR, "GetHeapPhysicalMemorySize(): fail to open the file");
        return -1;
    }
    const int bufSize = 256;
    char buf[bufSize] = { '\0' };
    while (fgets(buf, bufSize, file) != nullptr) {
        uint64_t startAddr = 0;
        uint64_t endAddr = 0;
        // expect 2 parameters are both written.
        constexpr int expectResult = 2;
        int ret = sscanf_s(buf, "%lx-%lx rw-p", &startAddr, &endAddr);
        if (ret == expectResult && startAddr <= GetHeap().GetStartAddress() && endAddr >= GetHeap().GetStartAddress()) {
            ssize_t physicalMemorySize = 0;
            uint64_t tmpStartAddr = endAddr;
            do {
                bool getPss = false;
                if (tmpStartAddr != endAddr) {
                    LOG(RTLOG_ERROR, "GetHeapPhysicalMemorySize(): fail to read the file");
                    return -2; // -2: Return value of exception.
                }
                do {
                    (void)fgets(buf, bufSize, file);
                    ssize_t size = 0;
                    if (sscanf_s(buf, "Pss:%zuKB", &size) == 1) {
                        physicalMemorySize += size;
                        getPss = true;
                    }
                } while (sscanf_s(buf, "%lx-%lx", &startAddr, &endAddr) != 2); // expect 2 parameters are both written.
                if (!getPss) {
                    LOG(RTLOG_ERROR, "GetHeapPhysicalMemorySize(): fail to read pss value");
                    return -2; // -2: Return value of exception.
                }
                tmpStartAddr = endAddr;
            } while (endAddr <= GetHeap().GetSpaceEndAddress());
            return physicalMemorySize * KB;
        }
    }
    LOG(RTLOG_ERROR, "GetHeapPhysicalMemorySize fail");
    return -2; // -2: Return value of exception.
}
#endif

FinalizerProcessor& HeapImpl::GetFinalizerProcessor() { return collectorResources.GetFinalizerProcessor(); }

CollectorResources& HeapImpl::GetCollectorResources() { return collectorResources; }

void HeapImpl::StopGCWork() { ZCollectedHeap::stop(); }

void HeapImpl::RegisterAllocBuffer(AllocBuffer& buffer) { GetAllocator().RegisterAllocBuffer(buffer); }

void HeapImpl::RemoveAllocBuffer(AllocBuffer &buffer) { GetAllocator().RemoveAllocBuffer(buffer); }

void HeapImpl::VisitAllExportRoots(const NativeSlotVisitor &visitor)
{
    exportRootsTable.VisitGCRoots(visitor);
}

BaseObject* HeapImpl::GetExportObject(U64 id)
{
    return exportRootsTable.GetExportRoot(id);
}

U64 HeapImpl::RegisterExportRoot(BaseObject *obj)
{
    if (!IsHeapAddress(obj)) {
        return std::numeric_limits<U64>::max();
    }
    return exportRootsTable.RegisterExportRoot(obj);
}

void HeapImpl::RemoveExportObject(U64 id)
{
    exportRootsTable.RemoveExportRoot(id);
}

void HeapImpl::CrossAccessBarrier(I64 id)
{
    BaseObject* recordObj = GetExportObject(id);
    if (recordObj == nullptr) {
        return;
    }
    // GetExportObject loads the native slot through its colored load barrier.
    // Preserve that current identity, including an in-place destination whose
    // address is also another object's from-key (ZUncoloredRoot::make_load_good,
    // zUncoloredRoot.inline.hpp:62-69). Page ownership cannot reclassify it.
    reinterpret_cast<CopyCollector&>(GetCollector()).ResurrectExportObject(recordObj);
    SetExportObjActiveState(id, true);
}

void HeapImpl::SetExportObjActiveState(U64 id, bool state)
{
    exportRootsTable.SetActiveState(id, state);
}

bool HeapImpl::CheckExportObjState(U64 id, BaseObject *exportObj)
{
    return exportRootsTable.CheckActiveState(id, exportObj);
}
} // namespace MapleRuntime

namespace MapleRuntime {
void RegionManager::ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                                     bool skipKnownEmptyRegions) const
{
    VisitPageOwners([&](ZPage* region) {
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            return;
        }
        if (skipKnownEmptyRegions && region->IsKnownEmpty()) {
            return;
        }
        region->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
    });
}

void RegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

void RegionManager::StampCensusBoundaries()
{
    VisitPageOwners([&](ZPage* region) {
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            region->StampCensusBoundary();
        }
    });
}

} // namespace MapleRuntime

namespace MapleRuntime {
ZPage* Heap::page(MAddress addr) { return ZPageTable::heap_table().get(addr); }

ZPageTable& Heap::page_table() { return ZPageTable::heap_table(); }

ZPage* Heap::alloc_page(size_t num, ZPageType role, bool expectPhysicalMem, bool allowSaferegion,
                             bool clearPayload, PageAge age)
{
    RegionManager& manager = static_cast<RegionSpace&>(GetHeap().GetAllocator()).GetRegionManager();
    ZPage* page = manager.TakeRegion(num, role, expectPhysicalMem, allowSaferegion, clearPayload, age);
    if (page != nullptr && page_table().get(page->GetRegionStart()) != page) {
        page_table().insert(page);
    }
    return page;
}

void Heap::free_page(ZPage* page)
{
    if (page == nullptr) {
        return;
    }
    ZPage::RetirePage(page, [] {});
}

size_t Heap::free_empty_pages(ZGenerationId id, const ZArray<ZPage*>* pages)
{
    (void)id;
    size_t freed = 0;
    if (pages == nullptr) {
        return 0;
    }
    for (int i = 0; i < pages->length(); ++i) {
        ZPage* page = pages->at(i);
        if (page == nullptr) {
            continue;
        }
        if (RegionList* owner = page->GetRegionListOwner()) {
            owner->DeleteRegion(page);
        }
        freed += page->size();
        free_page(page);
    }
    return freed;
}

bool Heap::is_in(MAddress addr)
{
    ZPage* p = page(addr);
    return p != nullptr && p->is_in(to_zaddress(addr));
}

bool Heap::is_young(MAddress addr)
{
    ZPage* p = page(addr);
    return p != nullptr && p->IsYoungRegion();
}

bool Heap::is_old(MAddress addr)
{
    ZPage* p = page(addr);
    return p != nullptr && !p->IsYoungRegion();
}

// heapDumper.cpp: VM_HeapDumper::doit. The requesting thread executes the
// safepoint operation; neither generation driver consumes inspector work.
void Heap::DumpHeap(HeapDumpKind kind)
{
    switch (kind) {
        case HeapDumpKind::NORMAL:
        case HeapDumpKind::OOM: {
            CjHeapData dump(kind == HeapDumpKind::OOM);
            dump.DumpHeap();
            break;
        }
        case HeapDumpKind::IDE: {
#if defined(__OHOS__) && (__OHOS__ == 1)
            CjHeapDataForIDE dump;
            dump.Serialize();
#endif
            break;
        }
        default:
            CHECK(false);
    }
}

void Heap::object_iterate(ObjectClosure* object_cl, bool visit_weaks)
{
    HeapIterator iter(visit_weaks, false, 1);
    iter.object_iterate([&](BaseObject* object) { object_cl->do_object(object); }, 0);
}

void Heap::object_and_field_iterate_for_verify(ObjectClosure* object_cl, bool visit_weaks)
{
    HeapIterator iter(visit_weaks, true, 1);
    iter.object_and_field_iterate([&](BaseObject* object) { object_cl->do_object(object); }, {}, 0);
}
}
