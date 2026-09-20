// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Common/RunType.h"
#include "Common/OopStorage.h"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zArray.hpp"

#include "Heap/z/zMark.hpp"
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

#include <new>
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/shared/collectedHeap.hpp"
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
Heap* Heap::_heap = nullptr;

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

Heap::Heap()
{
    _heap = this;
    RunType::InitRunTypeMap();
    _allocation_adapter.reset(new RegionSpace());
    exportRootsTable = new ExportRootTable();
    staticRootTable = new StaticRootTable();
    ZStat::NotifyHeapConstructed();
}

Heap::~Heap()
{
    delete exportRootsTable;
    exportRootsTable = nullptr;
    delete staticRootTable;
    staticRootTable = nullptr;
}





MAddress Heap::Allocate(size_t size, AllocType allocType) { return _allocation_adapter->Allocate(size, allocType); }

bool Heap::ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const
{
    return _allocation_adapter->ForEachObj(visitor, safe);
}

void Heap::Init(const HeapParam& param)
{
    ZArguments::initialize();
    ZHeuristics::set_max_heap_size(param.heapSize * 1024);
    ZInitialize::initialize();
    _page_allocator.Init(param);
    // zHeap.cpp:89-90: capacity bounds open both generations' heap accounts.
    // Host difference: HeapParam has no min-heap-size, min reports 0.
    young().StatHeap()->AtInitialize(0, _page_allocator.GetHeapCapacity());
    old().StatHeap()->AtInitialize(0, _page_allocator.GetHeapCapacity());
    Heap::GetHeap().EnableGC(ZArguments::gc_enabled());
    {
        young().forwarding_table().initialize();
        old().forwarding_table().initialize();
    }
    young().remembered()->bind(
        &page_table(),
        &old().forwarding_table(),
        &_page_allocator);
    // zCollectedHeap.cpp:initialize_gc_workers creates ZWorkers with the
    // ConcGCThreads budget (zWorkers.cpp:45-65). Do not pre-create a max=1
    // pool here: that made later set_active_workers(ConcGCThreads) fail the
    // WorkerThreads 1-max check (workerThread.cpp:148).
    ZCollectedHeap::heap()->initialize_gc();
    _initialized = true;
}

void Heap::Fini()
{
    ZCollectedHeap::heap()->finalize_gc();
    young().StopWorkers();
    old().StopWorkers();
}


void Heap::RequestGC(GCReason reason, bool async) { ZCollectedHeap::heap()->collect(reason, async); }

void Heap::ResolveCycleRef() { cross_vm().ResolveCycleRef(); }

void Heap::MarkYoungRootObject(BaseObject* object)
{
    // #596's barrier already established current and selected young. Keep the
    // generation mark-phase assertion at ZGeneration::mark_object's entry.
    auto& cycle = GetZGeneration(ZGenerationId::young);
    cycle.MarkObjectIfActive<false, true, true, false>(from_object(object));
}

void Heap::MarkObjectIfActive(BaseObject* object)
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    ZBarrier::Mark<false, false, true, false>(from_object(object));
}

void Heap::MarkYoungObjectIfActive(BaseObject* object)
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    GetZGeneration(ZGenerationId::young)
        .MarkObjectIfActive<false, false, true, false>(from_object(object));
}

void Heap::MarkNewObject(BaseObject* obj)
{
    // Registration follows object initialization (BaseObject::RegisterFinalizer).
    // ZMark::AnyThread / DontFollow: publish mark-only work for this current object.
    ZGeneration& cycle = GetZGeneration(ObjectGeneration(obj));
    cycle.MarkObjectIfActive<false, false, false, false>(from_object(obj));
}

BaseObject* Heap::make_load_good(RefField<>& ref, const ForwardingProvenance& provenance)
{
    return to_object(ZBarrier::make_load_good(ref.GetFieldValue(), provenance));
}

void Heap::PublishGenerationPhase(ZGenerationId generation, ZGenerationPhase value)
{
    ZGeneration& cycle = GetZGeneration(generation);
    const ZGenerationPhase before = cycle.GcPhase();
    if (generation == ZGenerationId::old &&
        value == ZGenerationPhase::Relocate && before != ZGenerationPhase::Relocate) {
        Heap::GetHeap().old().RecordYoungSequenceAtRelocateStart(Heap::GetHeap().young().Sequence());
    }
    cycle.PublishPhase(value);
}

Generation Heap::ObjectGeneration(BaseObject* object) const
{
    const MAddress address = reinterpret_cast<MAddress>(object);
    // ZHeap::is_young uses the current page, including after promotion.
    return Heap::page(address)->GetOwnerGeneration();
}

bool Heap::FlushGCDataMarkProducers(ThreadGCData& data)
{
    return ZMark::FlushGCDataMarkProducers(data);
}

bool Heap::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    return ZMark::FlushThreadMarkProducers(tls);
}


bool Heap::IsGhostFromObject(BaseObject* obj) const { return ZRelocate::IsFromObject(obj); }

bool Heap::IsUnmovableFromObject(BaseObject* obj) const { return ZRelocate::IsUnmovableFromObject(obj); }

BaseObject* Heap::ForwardObject(BaseObject* fromVersion, Generation generation)
{
    return ZRelocate::ForwardObject(fromVersion, generation);
}

BaseObject* Heap::relocate_or_remap_object(BaseObject* object, ZGenerationId generation)
{
    return GetZGeneration(generation).relocate_or_remap_object(object);
}

bool Heap::IsSurvivedObject(const BaseObject* obj) const
{
    return Heap::page(reinterpret_cast<MAddress>(obj))->is_object_live(from_object(obj));
}

bool Heap::IsGcStarted() const
{
    return GetCycleSnapshot(ZGenerationId::young).active || GetCycleSnapshot(ZGenerationId::old).active;
}

bool Heap::IsGCEnabled() const { return isGCEnabled.load(); }

void Heap::EnableGC(bool val) { isGCEnabled.store(val); }

OopStorage& Heap::GetExportRootStorage() { return exportRootsTable->RootStorage(); }

RegionSpace& Heap::GetAllocator() { return *_allocation_adapter; }

size_t Heap::GetMaxCapacity() const { return _page_allocator.GetHeapCapacity(); }

ZMemoryUsageInfo Heap::GetMemoryUsage() const
{
    const size_t young = _page_allocator.used_generation(ZGenerationId::young);
    const size_t old = _page_allocator.used_generation(ZGenerationId::old);
    return ComputeMemoryUsageInfo(_page_allocator.GetCommittedCapacity(), GetMaxCapacity(), young, old);
}


size_t Heap::GetCurrentCapacity() const { return _page_allocator.GetCommittedBytes(); }

size_t Heap::GetUsedPageSize() const { return _page_allocator.GetUsedRegionSize(); }

size_t Heap::GetAllocatedSize() const { return _page_allocator.GetAllocatedSize(); }

MAddress Heap::GetStartAddress() const { return _page_allocator.GetSpaceStartAddress(); }

MAddress Heap::GetSpaceEndAddress() const { return _page_allocator.GetSpaceEndAddress(); }

Heap& Heap::GetHeap()
{
    return ZCollectedHeap::heap()->collected_heap();
}

void Heap::install_page_table()
{
    _page_table = ZPageTable(ZAddressOffsetMax);
}

ZRemembered& Heap::remembered()
{
    return *young().remembered();
}

void Heap::RegisterStaticRoots(Uptr addr, U32 size)
{
    staticRootTable->RegisterRoots(reinterpret_cast<StaticRootTable::StaticRootArray*>(addr), size);
}

void Heap::UnregisterStaticRoots(Uptr addr, U32 size)
{
    staticRootTable->UnregisterRoots(reinterpret_cast<StaticRootTable::StaticRootArray*>(addr), size);
}

void Heap::VisitStaticRoots(const NativeSlotVisitor& visitor)
{
    staticRootTable->VisitRoots(visitor);
#ifdef INTERPRETER_ENABLED
    VisitInterpreterGlobalRoots(&visitor);
#endif
}

#if defined(_WIN64)
ssize_t Heap::GetHeapPhysicalMemorySize() const
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
ssize_t Heap::GetHeapPhysicalMemorySize() const
{
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count) != KERN_SUCCESS)
        return -2;  // -2: Return value of exception.
    return t_info.resident_size;
}

#else
ssize_t Heap::GetHeapPhysicalMemorySize() const
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

FinalizerProcessor& Heap::GetFinalizerProcessor() { return ZCollectedHeap::heap()->finalizer_processor(); }

void Heap::StopGCWork() { ZCollectedHeap::stop(); }

void Heap::RegisterAllocBuffer(AllocBuffer& buffer) { GetAllocator().RegisterAllocBuffer(buffer); }

void Heap::RemoveAllocBuffer(AllocBuffer &buffer) { GetAllocator().RemoveAllocBuffer(buffer); }

void Heap::VisitAllExportRoots(const NativeSlotVisitor &visitor)
{
    exportRootsTable->VisitGCRoots(visitor);
}

BaseObject* Heap::GetExportObject(U64 id)
{
    return exportRootsTable->GetExportRoot(id);
}

U64 Heap::RegisterExportRoot(BaseObject *obj)
{
    if (!IsHeapAddress(obj)) {
        return std::numeric_limits<U64>::max();
    }
    return exportRootsTable->RegisterExportRoot(obj);
}

void Heap::RemoveExportObject(U64 id)
{
    exportRootsTable->RemoveExportRoot(id);
}

void Heap::CrossAccessBarrier(I64 id)
{
    BaseObject* recordObj = GetExportObject(id);
    if (recordObj == nullptr) {
        return;
    }
    // GetExportObject loads the native slot through its colored load barrier.
    // Preserve that current identity, including an in-place destination whose
    // address is also another object's from-key (ZUncoloredRoot::make_load_good,
    // zUncoloredRoot.inline.hpp:62-69). Page ownership cannot reclassify it.
    cross_vm().ResurrectExportObject(recordObj);
    SetExportObjActiveState(id, true);
}

void Heap::SetExportObjActiveState(U64 id, bool state)
{
    exportRootsTable->SetActiveState(id, state);
}

bool Heap::CheckExportObjState(U64 id, BaseObject *exportObj)
{
    return exportRootsTable->CheckActiveState(id, exportObj);
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
ZPage* Heap::page(MAddress addr) { return page_table().get(addr); }

ZPageTable& Heap::page_table() { return GetHeap()._page_table; }

RegionManager* Heap::_test_page_allocator = nullptr;

void Heap::bind_test_page_allocator(RegionManager* manager)
{
    _test_page_allocator = manager;
}

RegionManager& Heap::page_allocator()
{
    if (_test_page_allocator != nullptr) {
        return *_test_page_allocator;
    }
    return _page_allocator;
}

const RegionManager& Heap::page_allocator() const
{
    if (_test_page_allocator != nullptr) {
        return *_test_page_allocator;
    }
    return _page_allocator;
}

namespace {
thread_local ZPage* g_prematerializedPage = nullptr;
}

ZPage* Heap::alloc_page(ZPage* page)
{
    g_prematerializedPage = page;
    ZPage* published = alloc_page(0, ZPageType::small, false, false, false);
    g_prematerializedPage = nullptr;
    return published;
}

// ZGC zHeap.cpp:148-160; MinTLABSize is in bytes in this runtime.
size_t Heap::unsafe_max_tlab_alloc() const
{
    size_t size = _object_allocator.fast_available(PageAge::eden);
    if (size < 2 * 1024) { size = max_tlab_size(); }
    return std::min(size, max_tlab_size());
}

// ZGC zHeap.inline.hpp:92-95: TLABs use the same object allocator.
uintptr_t Heap::alloc_tlab(size_t size)
{
    CHECK(size <= ZObjectSizeLimitSmall);
    return object_allocator().alloc(size, PageAge::eden);
}

ZPage* Heap::alloc_page(size_t num, ZPageType role, bool expectPhysicalMem, bool allowSaferegion,
                             bool clearPayload, PageAge age, ZAllocationFlags flags)
{
    RegionManager& manager = GetHeap().page_allocator();
    ZPage* page = g_prematerializedPage;
    if (page == nullptr && num > 0) {
        page = manager.TakeRegion(num, role, expectPhysicalMem, allowSaferegion, clearPayload, age, flags);
    }
    if (page != nullptr) {
        page_table().insert(page);
    }
    return page;
}

void Heap::free_page(ZPage* page)
{
    if (page == nullptr) {
        return;
    }
    page_table().remove(page);
    GetHeap().page_allocator().free_page(page);
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
        // #710: select_relocation_set owns candidacy; freeing clears the role.
        page->SetRegionRole(ZPageRole::None);
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

namespace MapleRuntime {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void Heap::DumpHeap(const CString& tag)
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "Not In STW");
    DLOG(FRAGMENT, "DumpHeap %s", tag.Str());
    // dump roots
    DumpRoots(FRAGMENT);
    // dump object contents
    auto dumpVisitor = [](BaseObject* obj) { obj->DumpObject(FRAGMENT); };
    bool ret = Heap::GetHeap().ForEachObj(dumpVisitor, false);
    CHECK_E(UNLIKELY(!ret), "theAllocator.ForEachObj() in DumpHeap() return false.");

    // dump object types
    DLOG(FRAGMENT, "Print Type information");
    std::set<TypeInfo*> classinfoSet;
    auto assembleClassInfoVisitor = [&classinfoSet](BaseObject* obj) {
        TypeInfo* classInfo = obj->GetTypeInfo();
        // No need to check the result of insertion, because there are multiple-insertions.
        (void)classinfoSet.insert(classInfo);
    };
    ret = Heap::GetHeap().ForEachObj(assembleClassInfoVisitor, false);
    CHECK_E(UNLIKELY(!ret), "theAllocator.ForEachObj()#2 in DumpHeap() return false.");

    for (auto it = classinfoSet.begin(); it != classinfoSet.end(); it++) {
        TypeInfo* classInfo = *it;
        DLOG(FRAGMENT, "%p %s", classInfo, classInfo->GetName());
    }
    DLOG(FRAGMENT, "Dump Allocator");
}
#endif

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void Heap::DumpRoots(LogType logType)
{
    RootVisitor rootVisitor = [this, logType](ObjectRef& ref) {
        zaddress_unsafe value = ref.LoadPlain();
        if (is_null(value)) {
            return;
        }
        // DumpRoots is called while the root owner retains the target for inspection.
        auto obj = to_object(safe(value));
        DLOG(logType, "%p Fast Check %d Accurate Check %d", obj,
              Heap::GetHeap().GetAllocator().IsHeapAddress(reinterpret_cast<MAddress>(obj)),
              Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(obj)));
    };

    DLOG(logType, "stack roots");
    MutatorManager::Instance().VisitAllMutators(
        [&rootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rootVisitor); });

    DLOG(logType, "finalizer processor roots");

    NativeSlotVisitor rootSlotVisitor = [this, logType](NativeSlot& ref) {
        zpointer value = ref.GetFieldValue();
        if (is_null(value)) {
            return;
        }
        // StaticRootTable keeps the referent live while DumpRoots inspects it.
        auto obj = ZBarrier::ReadStaticRef(ref);
        if (obj == nullptr) {
            return;
        }
        DLOG(logType, "%p Fast Check %d Accurate Check %d", obj,
              Heap::GetHeap().GetAllocator().IsHeapAddress(reinterpret_cast<MAddress>(obj)),
              Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(obj)));
    };

    DLOG(logType, "static fields");
    Heap::GetHeap().GetFinalizerProcessor().VisitGCRoots(rootSlotVisitor);
    ZMark::VisitStaticRoots(rootSlotVisitor);

    DLOG(logType, "Dump GCRoots end");
}
#endif

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void Heap::DumpBeforeGC()
    {
        if (ENABLE_LOG(FRAGMENT)) {
            if (MutatorManager::Instance().WorldStopped()) {
                DumpHeap("before_gc");
            } else {
                ScopedStopTheWorld stw("dump before gc");
                DumpHeap("before_gc");
            }
        }
    }
#endif

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void Heap::DumpAfterGC()
    {
        if (ENABLE_LOG(FRAGMENT)) {
            if (MutatorManager::Instance().WorldStopped()) {
                DumpHeap("after_gc");
            } else {
                ScopedStopTheWorld stw("dump after gc");
                DumpHeap("after_gc");
            }
        }
    }
#endif
}
