// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#include "gc_heap_fixture.hpp"
#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Heap/Collector/RelocationRequestQueue.h"
#include "Heap/Verify/FromPageDetachCheck.h"
#include "Heap/GcThreadPool.h"
#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/PreForwardBaseMap.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "ObjectModel/RefField.inline.h"
#include "ObjectModel/MArray.inline.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }

    static void ReleaseListOwnership(RegionInfo* region)
    {
        RegionList* owner = region == nullptr ? nullptr : region->GetRegionListOwner();
        if (owner != nullptr) {
            owner->DeleteRegion(region);
        }
        GC_EXPECT_TRUE(region == nullptr || region->GetRegionListOwner() == nullptr);
    }

    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static void BindRuntimeWorkers(CollectorResources& resources, RuntimeWorkers* threadPool)
    {
        resources.runtimeWorkers = threadPool;
    }

    static void Exempt(RegionManager& manager, RegionInfo* region)
    {
        manager.ExemptFromRegion(region);
    }

    static RefField<> QualifyStoreValue(WCollector& collector, BaseObject* value)
    {
        return collector.GetAndTryTagRefField(value);
    }

    static BaseObject* ResolveStoreValue(WCollector& collector, BaseObject* value)
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, value, &value };
        return collector.ResolveStoreValue(value, provenance);
    }

    static void CheckStoreGoodTarget(WCollector& collector, BaseObject* value)
    {
        collector.CheckStoreGoodTarget("ForwardingLookupWitness", value,
            ForwardingProvenance{ ForwardingHolderKind::HeapRef, value, &value });
    }

    static BaseObject* ForwardUpdateRawRef(WCollector& collector, ObjectRef& root)
    {
        return collector.ForwardUpdateRawRef(root);
    }

    static bool FixMinorField(WCollector& collector, RefField<>& field, BaseObject* knownBase = nullptr)
    {
        return collector.FixMinorEvacuatedSlot(field, knownBase, nullptr, false);
    }

    static bool FixMinorRoot(WCollector& collector, RootSlot& root)
    {
        return collector.FixMinorEvacuatedSlot(root, nullptr);
    }

    static BaseObject* TryForward(WCollector& collector, BaseObject* object)
    {
        return collector.TryForwardObject(object);
    }

    static BaseObject* WaitRoutedTipReady(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* forwarding)
    {
        (void)to;
        RegionInfo::RetainScope lease(forwarding);
        return collector.ForwardObjectImpl(from, forwarding, lease);
    }

    static bool TryUpdateRefField(WCollector& collector, BaseObject* obj, RefField<>& field, BaseObject*& newRef)
    {
        return collector.TryUpdateRefField(obj, field, newRef);
    }

    static BaseObject* ProductGetForwardPointer(
        WCollector& collector, BaseObject* from, RegionInfo* forwarding)
    {
        using ProductFn = BaseObject* (*)(const WCollector*, BaseObject*, RegionInfo*);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        void* symbol = handle == nullptr ? nullptr : dlsym(
            handle, "_ZNK12MapleRuntime10WCollector17GetForwardPointerEPNS_10BaseObjectEPNS_10RegionInfoE");
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        BaseObject* result = symbol == nullptr ? nullptr :
            reinterpret_cast<ProductFn>(symbol)(&collector, from, forwarding);
        if (handle != nullptr) {
            (void)dlclose(handle);
        }
        return result;
    }

    static FindToVersionResult ProductFindToVersion(WCollector& collector, BaseObject* from)
    {
        using ProductFn = FindToVersionResult (*)(const WCollector*, BaseObject*);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        void* symbol = handle == nullptr ? nullptr : dlsym(
            handle, "_ZNK12MapleRuntime10WCollector13FindToVersionEPNS_10BaseObjectE");
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        if (info.dli_fname != nullptr) {
            std::fprintf(stderr, "FINDTO_PRODUCT_SO=%s\n", info.dli_fname);
        }
        FindToVersionResult result = symbol == nullptr
            ? FindToVersionResult::NotManaged()
            : reinterpret_cast<ProductFn>(symbol)(&collector, from);
        if (handle != nullptr) {
            (void)dlclose(handle);
        }
        return result;
    }

    static BaseObject* ProductRelocateOrRemap(
        WCollector& collector, BaseObject* from, ZGenerationId generation)
    {
        using ProductFn = BaseObject* (*)(const WCollector*, BaseObject*, ZGenerationId);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        void* symbol = handle == nullptr ? nullptr : dlsym(
            handle,
            "_ZNK12MapleRuntime10WCollector24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdE");
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        BaseObject* result = symbol == nullptr ? nullptr :
            reinterpret_cast<ProductFn>(symbol)(&collector, from, generation);
        if (handle != nullptr) {
            (void)dlclose(handle);
        }
        return result;
    }

    static BaseObject* ForwardExclusive(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* copyPage)
    {
        return collector.ForwardObjectExclusive(from, to, copyPage);
    }

    static BaseObject* ForwardImpl(WCollector& collector, BaseObject* from, RegionInfo* copyPage)
    {
        RegionInfo::RetainScope lease(copyPage);
        return lease.ok() ? collector.ForwardObjectImpl(from, copyPage, lease) : nullptr;
    }

    static void RemapYoungRoots(WCollector& collector) { collector.RemapYoungRoots(); }

    static void SeedValueRoots(WCollector& collector, BaseObject* value)
    {
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            collector.resurrectedExportObjectes.clear();
            collector.resurrectedExportObjectesForwardPhase.clear();
            collector.resurrectedExportObjectes.insert(value);
            collector.resurrectedExportObjectesForwardPhase.insert(value);
        }
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        collector.cycleRefWorkStack.clear();
        collector.cycleRefWorkStack[value].push_back(value);
    }

    static bool AllValueRootCarriersEqual(WCollector& collector, BaseObject* value)
    {
        bool resurrected = false;
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            resurrected = collector.resurrectedExportObjectes.size() == 1 &&
                collector.resurrectedExportObjectes.count(value) == 1 &&
                collector.resurrectedExportObjectesForwardPhase.size() == 1 &&
                collector.resurrectedExportObjectesForwardPhase.count(value) == 1;
        }
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        auto it = collector.cycleRefWorkStack.find(value);
        return resurrected && collector.cycleRefWorkStack.size() == 1 &&
            it != collector.cycleRefWorkStack.end() && it->second.size() == 1 &&
            it->second.front() == value;
    }

    static bool BothResurrectionSetsEqual(WCollector& collector, BaseObject* value)
    {
        std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
        return collector.resurrectedExportObjectes.size() == 1 &&
            collector.resurrectedExportObjectes.count(value) == 1 &&
            collector.resurrectedExportObjectesForwardPhase.size() == 1 &&
            collector.resurrectedExportObjectesForwardPhase.count(value) == 1;
    }

    static std::vector<BaseObject*> VisitMinorValueRoots(WCollector& collector)
    {
        std::vector<BaseObject*> visited;
        collector.VisitMinorValueRoots([&visited](BaseObject* value) { visited.push_back(value); });
        return visited;
    }

    static std::vector<BaseObject*> EnumMajorValueRoots(WCollector& collector)
    {
        TracingCollector::RootSet rootSet;
        collector.EnumAllSurrectedExportRoots(rootSet);
        std::vector<BaseObject*> visited;
        while (!rootSet.empty()) {
            visited.push_back(rootSet.back().object());
            rootSet.pop_back();
        }
        return visited;
    }

    static void RunLateValueRootRekey(WCollector& collector)
    {
        collector.PreforwardDiscoveredExternObjects();
        collector.PreforwardAllResurrectExportFromObjects();
    }
};

// The four delivery fixtures enter the same private product methods that their
// runtime callers use.  Friendship is enabled only for MRT_TESTABLE_INTERNALS;
// it adds no product export, branch, or runtime state.
struct LoadHealDeliveryTestAccess {
    struct RemsetConsumeResult {
        size_t work;
        size_t consumed;
    };

    static void PublishColours(WCollector& collector) { collector.set_good_masks(); }

    static uintptr_t DoubleBadColour(const WCollector& collector)
    {
        return REMAP_COLOUR_MASK & ~collector.ZPointerRemappedYoungMask &
            ~collector.ZPointerRemappedOldMask;
    }

    static void RemapYoungRoots(WCollector& collector) { collector.RemapYoungRoots(); }

    static void FlipYoungRelocateStart(WCollector& collector)
    {
        collector.flip_young_relocate_start();
    }

    static void FlipOldRelocateStart(WCollector& collector)
    {
        collector.flip_old_relocate_start();
    }

    static RemsetConsumeResult ConsumeRemembered(WCollector& collector,
                                                  const std::unordered_set<MAddress>& previous,
                                                  BaseObject* currentMinorRoot)
    {
        collector.flip_young_mark_start();
        WCollector::WorkStack workStack = collector.NewWorkStack();
        WCollector::MinorSlotSet reachableSlots;
        WCollector::MinorSlotSet weakSlots;
        WCollector::MinorObjectSet currentMinorRoots;
        WCollector::MinorSlotSet consumed;
        RemsetScanStats stats;
        stats.recorded = previous.size();
        if (currentMinorRoot != nullptr) {
            currentMinorRoots.insert(currentMinorRoot);
        }
        collector.RescanRememberedSet(workStack, previous, reachableSlots, weakSlots,
                                      currentMinorRoots, false, &consumed, &stats);
        const size_t work = workStack.size();
        while (!workStack.empty()) {
            workStack.pop_back();
        }
        return RemsetConsumeResult { work, consumed.size() };
    }
};

} // namespace MapleRuntime

namespace {

struct CopyAdmissionBarrier {
    static void Reset(BaseObject* object = nullptr)
    {
        std::lock_guard<std::mutex> guard(mu);
        target = object;
        entered = false;
        released = false;
    }

    static void Hook(RegionInfo*, BaseObject* object)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (target != nullptr && object != target) {
            return;
        }
        entered = true;
        cv.notify_all();
        cv.wait(lock, []() { return released; });
    }

    static void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, []() { return entered; });
    }

    static void Release()
    {
        std::lock_guard<std::mutex> guard(mu);
        released = true;
        cv.notify_all();
    }

    static std::mutex mu;
    static std::condition_variable cv;
    static BaseObject* target;
    static bool entered;
    static bool released;
};

std::mutex CopyAdmissionBarrier::mu;
std::condition_variable CopyAdmissionBarrier::cv;
BaseObject* CopyAdmissionBarrier::target = nullptr;
bool CopyAdmissionBarrier::entered = false;
bool CopyAdmissionBarrier::released = false;

struct PageWaitEnterBarrier {
    static void Reset()
    {
        std::lock_guard<std::mutex> guard(mu);
        entered = false;
    }
    static void Hook(ZForwarding* forwarding)
    {
        GC_EXPECT_TRUE(forwarding != nullptr);
        GC_EXPECT_TRUE(!forwarding->is_done());
        std::lock_guard<std::mutex> guard(mu);
        entered = true;
        cv.notify_all();
    }
    static void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, []() { return entered; });
    }
    static std::mutex mu;
    static std::condition_variable cv;
    static bool entered;
};
std::mutex PageWaitEnterBarrier::mu;
std::condition_variable PageWaitEnterBarrier::cv;
bool PageWaitEnterBarrier::entered = false;

struct CopyCompletionBarrier {
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    static void Hook(void* context)
    {
        auto& barrier = *static_cast<CopyCompletionBarrier*>(context);
        std::unique_lock<std::mutex> lock(barrier.mu);
        barrier.entered = true;
        barrier.cv.notify_all();
        barrier.cv.wait(lock, [&barrier]() { return barrier.released; });
    }

    void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [this]() { return entered; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> guard(mu);
        released = true;
        cv.notify_all();
    }
};

struct CopyAdmissionWitness {
    static void Reset() { hits.store(0, std::memory_order_relaxed); }

    static void Hook(RegionInfo*, BaseObject*) { hits.fetch_add(1, std::memory_order_relaxed); }

    static uint32_t Hits() { return hits.load(std::memory_order_relaxed); }

    static std::atomic<uint32_t> hits;
};

std::atomic<uint32_t> CopyAdmissionWitness::hits { 0 };

using ProductSetCopyAdmissionTestHook = void (*)(void (*)(RegionInfo*, BaseObject*));
using ProductForcePublicationClosedForTest = void (*)(MAddress);

ProductSetCopyAdmissionTestHook ProductSetCopyAdmissionTestHookFn()
{
    void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        handle = dlopen("libcangjie-runtime.so", RTLD_NOW);
    }
    return handle == nullptr ? nullptr : reinterpret_cast<ProductSetCopyAdmissionTestHook>(
        dlsym(handle, "MRT_SetCopyAdmissionTestHook"));
}

ProductForcePublicationClosedForTest ProductForcePublicationClosedForTestFn()
{
    void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        handle = dlopen("libcangjie-runtime.so", RTLD_NOW);
    }
    return handle == nullptr ? nullptr : reinterpret_cast<ProductForcePublicationClosedForTest>(
        dlsym(handle, "_ZN12MapleRuntime15ForwardingTable29ForcePublicationClosedForTestEm"));
}

class ResolveBarrier final : public Barrier {
public:
    ResolveBarrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet)
    {
    }

    BaseObject* Resolve(BaseObject* from) const
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, from, &from };
        return ResolveFromCopyForMutator(from, provenance);
    }
};

GcHeapFixture& ProductFixture()
{
    static GcHeapFixture fixture;
    static const bool initialized = ForwardingTable::Initialize(
        fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    // CompactRegion now carries remembered bits with an in-place copy.  This
    // independent product-test process does not run Heap::Init, so initialize
    // the Heap-owned remembered set alongside its forwarding table.
    static const bool rememberedInitialized = [&]() {
        Heap::GetHeap().GetRememberedSet().Initialize(
            fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        return true;
    }();
    GC_EXPECT_TRUE(initialized);
    GC_EXPECT_TRUE(rememberedInitialized);
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    return fixture;
}

class LoadHealDeliveryRuntime final : public Runtime {
public:
    static void Ensure()
    {
        static LoadHealDeliveryRuntime runtimeContainer;
        (void)runtimeContainer;
    }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}

private:
    LoadHealDeliveryRuntime()
    {
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        const ConcurrencyParam concurrencyParam = { 1024, 64, 1 };
        concurrency.Init(concurrencyParam);
    }

    MutatorManager manager;
    Concurrency concurrency;
};

struct DeliveryReferenceArrayTypes {
    DeliveryReferenceArrayTypes()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

DeliveryReferenceArrayTypes& GetDeliveryReferenceArrayTypes()
{
    static DeliveryReferenceArrayTypes types;
    return types;
}

void PinOwnerGeneration(RegionInfo* region, Generation gen)
{
    region->SetYoungRegionFlag(gen == Generation::Young ? 1 : 0);
}

void PublishGenerationMarkComplete(Generation gen)
{
    Heap::GetHeap().GetCollector().PublishGenerationPhase(
        gen == Generation::Old ? GCCycleGeneration::OLD : GCCycleGeneration::YOUNG, GC_PHASE_MARK_COMPLETE);
}

LiveInfo* PrepareForwardable(GcHeapFixture& fx, RegionInfo* region, MAddress liveObject)
{
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(liveObject);
    BaseObject* object = reinterpret_cast<BaseObject*>(liveObject);
    (void)bitmap->MarkBits(offset, object->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(object->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    // This synthetic fixture leaves an unmaterialized allocation prefix.
    // Record the known object start explicitly; production freezes a dense
    // allocation walk inside PrepareForwardableRegion.
    region->RecordRouteStart(offset);
    return live;
}

void DestroyAfterGhostCleared(RegionInfo* region, const char* why)
{
    if (region != nullptr && region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    ForwardingTable::ReclaimRetired(why);
}

struct LateBackfillState {
    RegionInfo* region;
    RegionInfo* destination;
    BaseObject* from;
    BaseObject* to;
    LiveInfo* live;
    uint64_t generation;
};

LateBackfillState PrepareLateBackfill(GcHeapFixture& fx, WCollector& collector)
{
    RegionInfo* region = RegionInfo::InitRegion(5, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);

    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    region->MarkForwardingDone();
    from->SetStateCode(ObjectState::FORWARDED);
    ZForwarding* table = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(table != nullptr);
    return LateBackfillState{ region, destination, from, to, live,
                              table == nullptr ? 0 : table->publication_generation() };
}

void CleanupLateBackfill(GcHeapFixture& fx, LateBackfillState& state)
{
    // The scenario has consumed its receipt. Normalize the planted header
    // before asking product retirement to prove no source still needs it.
    state.from->SetStateCode(ObjectState::NORMAL);
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    state.region->metadata.liveInfo = nullptr;
    fx.FreePlanted(state.live);
}

uint64_t RetireAnotherEmptyCarrier(LateBackfillState& state)
{
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        state.region->GetRegionStart(), state.region->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        state.region->GetRegionStart(), state.region->GetRegionSize(), state.region));
    ZForwarding* table = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_TRUE(table != nullptr);
    const uint64_t generation = table == nullptr ? 0 : table->publication_generation();
    GC_EXPECT_NE(generation, state.generation);
    GC_EXPECT_TRUE(ForwardingTable::PublishFromPageView(
        state.region, state.live, state.region->GetSnapshotEpoch(),
        state.region->GetRegionAllocPtr(), state.region->GetMarkStartAllocPtr(),
        state.region->GetLiveByteCount(), 1, 0, state.region->GetRegionLifeId()));
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    return generation;
}

size_t CountSubstring(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

struct PartialCompactState {
    RegionInfo* region;
    RegionInfo* destination;
    BaseObject* liveObject;
    LiveInfo* live;
    size_t objectSize;
};

PartialCompactState PreparePartialCompact(GcHeapFixture& fx, WCollector& collector, bool exhaustDestination)
{
    RegionInfo* region = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);

    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + objectSize);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + objectSize);
    destination->SetRegionAllocPtr(
        exhaustDestination ? destination->GetRegionEnd() : destination->GetRegionStart());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    return PartialCompactState{ region, destination, liveObject, live, objectSize };
}

void CleanupPartialCompact(GcHeapFixture& fx, PartialCompactState& state)
{
    RelocationReceiptTestAccess::ReleaseListOwnership(state.region);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    state.region->metadata.liveInfo = nullptr;
    fx.FreePlanted(state.live);
}

RegionInfo* ResetDeliveryUnit(GcHeapFixture& fx, size_t index)
{
    RegionInfo* previous = RegionInfo::GetRegionInfo(index);
    if (previous != nullptr) {
        RelocationReceiptTestAccess::ReleaseListOwnership(previous);
        if (previous->IsYoungRegion()) {
            previous->SetYoungRegionFlag(0);
        }
    }
    RegionInfo* region = RegionInfo::InitRegion(index, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(region->GetRegionStart());
    (void)fx;
    return region;
}

RememberedSet& DeliveryRememberedSet(GcHeapFixture& fx)
{
    RememberedSet& remembered = Heap::GetHeap().GetRememberedSet();
    (void)fx;
    return remembered;
}

class DeliveryNoAllocBufferScope final {
public:
    DeliveryNoAllocBufferScope() : saved(ThreadLocal::GetAllocBuffer())
    {
        ThreadLocal::SetAllocBuffer(nullptr);
    }

    ~DeliveryNoAllocBufferScope()
    {
        ThreadLocal::SetAllocBuffer(saved);
    }

private:
    AllocBuffer* saved;
};

void EmptyBothRememberedFaces(RememberedSet& remembered)
{
    std::unordered_set<MAddress> discarded;
    remembered.DrainForMinor(discarded);
    discarded.clear();
    remembered.DrainForMinor(discarded);
}

LateBackfillState PrepareValueRootForwarding(GcHeapFixture& fx, WCollector& collector)
{
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(
        state.region, reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                     publication, reinterpret_cast<MAddress>(state.from),
                     reinterpret_cast<MAddress>(state.to)),
                 reinterpret_cast<MAddress>(state.to));
    return state;
}

bool AllVisitedEqual(const std::vector<BaseObject*>& visited, BaseObject* expected)
{
    if (visited.size() != 4) {
        return false;
    }
    for (BaseObject* value : visited) {
        if (value != expected) {
            return false;
        }
    }
    return true;
}

void CompleteValueRootCoverage()
{
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    ForwardingTable::ReclaimRetired("value-root-mark-coverage");
}

} // namespace

GC_OTHER_VM_TEST(ValueRootCurrentization, MinorConsumerRewritesEveryCarrierBeforeCoverage)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareValueRootForwarding(fx, collector);
    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);

    const std::vector<BaseObject*> first =
        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    const bool consumerCurrent = AllVisitedEqual(first, state.to);
    const bool carrierCurrent =
        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);

    CleanupLateBackfill(fx, state);
    CompleteValueRootCoverage();
    const ForwardingTable::LookupResult afterCoverage =
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from));
    const std::vector<BaseObject*> afterReclaim =
        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    const bool independentAfterReclaim = AllVisitedEqual(afterReclaim, state.to);
    std::fprintf(stderr,
                 "VALUE_ROOT_TARGET_ASSERT minor consumer_current=%d carrier_current=%d "
                 "after_reclaim=%d lookup=%u\n",
                 static_cast<int>(consumerCurrent), static_cast<int>(carrierCurrent),
                 static_cast<int>(independentAfterReclaim), static_cast<unsigned>(afterCoverage.answer));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    GC_EXPECT_TRUE(consumerCurrent);
    GC_EXPECT_TRUE(carrierCurrent);
    GC_EXPECT_TRUE(independentAfterReclaim);
    GC_EXPECT_TRUE(afterCoverage.answer == ForwardingTable::ToAnswer::Unavailable);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorConsumerRewritesEveryCarrierBeforeCoverage)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareValueRootForwarding(fx, collector);
    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);

    const std::vector<BaseObject*> first =
        RelocationReceiptTestAccess::EnumMajorValueRoots(collector);
    const bool consumerCurrent = AllVisitedEqual(first, state.to);
    const bool carrierCurrent =
        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);

    CleanupLateBackfill(fx, state);
    CompleteValueRootCoverage();
    const ForwardingTable::LookupResult afterCoverage =
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from));
    const std::vector<BaseObject*> afterReclaim =
        RelocationReceiptTestAccess::EnumMajorValueRoots(collector);
    const bool independentAfterReclaim = AllVisitedEqual(afterReclaim, state.to);
    std::fprintf(stderr,
                 "VALUE_ROOT_TARGET_ASSERT major consumer_current=%d carrier_current=%d "
                 "after_reclaim=%d lookup=%u\n",
                 static_cast<int>(consumerCurrent), static_cast<int>(carrierCurrent),
                 static_cast<int>(independentAfterReclaim), static_cast<unsigned>(afterCoverage.answer));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    GC_EXPECT_TRUE(consumerCurrent);
    GC_EXPECT_TRUE(carrierCurrent);
    GC_EXPECT_TRUE(independentAfterReclaim);
    GC_EXPECT_TRUE(afterCoverage.answer == ForwardingTable::ToAnswer::Unavailable);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, InsertionAndLateRekeyShareCurrentAuthority)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareValueRootForwarding(fx, collector);

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    collector.ResurrectExportObject(state.from);
    collector.SetGCPhase(GCPhase::GC_PHASE_PREFORWARD);
    collector.ResurrectExportObject(state.from);
    const bool insertCurrent =
        RelocationReceiptTestAccess::BothResurrectionSetsEqual(collector, state.to);

    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);
    RelocationReceiptTestAccess::RunLateValueRootRekey(collector);
    const bool lateCurrent =
        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);
    std::fprintf(stderr,
                 "VALUE_ROOT_TARGET_ASSERT insertion_current=%d late_rekey_current=%d\n",
                 static_cast<int>(insertCurrent), static_cast<int>(lateCurrent));

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GC_EXPECT_TRUE(insertCurrent);
    GC_EXPECT_TRUE(lateCurrent);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, NullAndNonHeapControlsRemainStable)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);

    RelocationReceiptTestAccess::SeedValueRoots(collector, nullptr);
    const std::vector<BaseObject*> nullValues =
        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    BaseObject* nonHeap = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(1));
    RelocationReceiptTestAccess::SeedValueRoots(collector, nonHeap);
    const std::vector<BaseObject*> nonHeapValues =
        RelocationReceiptTestAccess::EnumMajorValueRoots(collector);
    const bool nullStable = AllVisitedEqual(nullValues, nullptr);
    const bool nonHeapStable = AllVisitedEqual(nonHeapValues, nonHeap);
    std::fprintf(stderr,
                 "VALUE_ROOT_CONTROL_ASSERT null_stable=%d nonheap_stable=%d\n",
                 static_cast<int>(nullStable), static_cast<int>(nonHeapStable));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    GC_EXPECT_TRUE(nullStable);
    GC_EXPECT_TRUE(nonHeapStable);
}

GC_TEST(ForwardingPublicationProduct, BarrierResolvesForwardedFromThroughCollector)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    RememberedSet rememberedSet;
    rememberedSet.Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    ResolveBarrier barrier(collector, rememberedSet);

    BaseObject* resolved = barrier.Resolve(state.from);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(resolved), reinterpret_cast<uintptr_t>(state.to));

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// ZGC zRelocate.cpp:382-409: the mutator runtime entry itself must retain the
// forwarding page and perform the first copy before falling back to a worker.
// ResolveBarrier's completed-route case above returns at WCollector.h:448-454;
// call the exported product entry here so that this arm cannot borrow that fast
// return or a test-ELF inline definition.
#if defined(MRT_FORWARDING_PUBLICATION_HOOKS_AVAILABLE)
GC_TEST(ForwardingPublicationProduct, MutatorRuntimeEntryReachesCopyAdmission)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);

    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    const size_t objectSize = from->GetSize();
    BaseObject* expected = fx.PlaceObject(destination->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + objectSize);
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(expected) + objectSize);

    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(from)),
                           objectSize, region->GetRegionSize());
    region->AddLiveByteCount(objectSize);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->RecordRouteStart(region->GetAddressOffset(reinterpret_cast<MAddress>(from)));
    region->MarkForwardingDone();
    AllocBuffer::GetOrCreateAllocBuffer()->SetRegion(destination);

    const MAddress fromAddress = reinterpret_cast<MAddress>(from);

    BaseObject* resolved = RelocationReceiptTestAccess::ProductRelocateOrRemap(
        collector, from, region->generation_id());

    const bool published = ForwardingTable::FindTo(fromAddress) != 0;
    const bool headerForwarded = from->IsForwarded();
    const MAddress receipt = ForwardingTable::FindTo(fromAddress);
    const int32_t copyCount = region->metadata.copyInflight.load(std::memory_order_acquire);

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-mutator-entry");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);

    GC_EXPECT_TRUE(resolved == expected);
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(headerForwarded);
    GC_EXPECT_EQ(receipt, reinterpret_cast<MAddress>(expected));
    GC_EXPECT_EQ(copyCount, 0);
}
#endif

GC_TEST(ForwardingNoGeometry, ForwardImplTryLockCopiesWithoutPrebuiltMapping)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    const size_t objectSize = from->GetSize();
    BaseObject* seedTo = fx.PlaceObject(destination->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + objectSize);
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(seedTo) + objectSize);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(from)),
                           objectSize, region->GetRegionSize());
    region->AddLiveByteCount(objectSize);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->RecordRouteStart(region->GetAddressOffset(reinterpret_cast<MAddress>(from)));
    region->MarkForwardingDone();
    AllocBuffer::GetOrCreateAllocBuffer()->SetRegion(destination);
    /*deleted copy SM*/ (void)(region->metadata.copyInflight);
    const MAddress fromAddress = reinterpret_cast<MAddress>(from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddress), static_cast<MAddress>(0));
    BaseObject* relocated = RelocationReceiptTestAccess::ForwardImpl(collector, from, region);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    AllocBuffer::GetOrCreateAllocBuffer()->ClearRegion();
    const bool moved = relocated != nullptr && relocated != from;
    const bool valid = relocated != nullptr && relocated->IsValidObject();
    const bool published = ForwardingTable::FindTo(fromAddress) == reinterpret_cast<MAddress>(relocated);
    const bool forwarded = from->IsForwarded();
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-forward-impl-trylock");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    GC_EXPECT_TRUE(moved);
    GC_EXPECT_TRUE(valid);
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(forwarded);
}

GC_TEST(ForwardingPublicationProduct, LateWaitBackfillCannotReopenSealedGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    const MAddress hit = ForwardingTable::FindTo(reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_TRUE(hit == 0);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from)) == nullptr);
    ForwardingTable::Publication late =
        ForwardingTable::RetainOpenPublicationAfterCopy(state.region, reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_FALSE(static_cast<bool>(late));

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, LateGetForwardPointerCannotReopenSealedGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    BaseObject* resolved =
        RelocationReceiptTestAccess::ProductGetForwardPointer(collector, state.from, state.region);
    GC_EXPECT_TRUE(resolved == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from)) == nullptr);
    ForwardingTable::Publication late =
        ForwardingTable::RetainOpenPublicationAfterCopy(state.region, reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_FALSE(static_cast<bool>(late));

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, LateFindToVersionCannotReopenSealedGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    FindToVersionResult resolved = RelocationReceiptTestAccess::ProductFindToVersion(collector, state.from);
    // Sealed generation, FORWARDED header, no InsertMapping. The retired table
    // is still present (ClearEntries has not destroyed it). Unavailable here
    // means never-installed for this from, not "table lifetime too short".
    GC_EXPECT_TRUE(resolved.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(resolved.unavailable_lookup_publication_closed());
    GC_EXPECT_TRUE(std::strstr(resolved.unavailable_lookup_cause(), "never_installed") != nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from)) == nullptr);
    ForwardingTable::Publication late =
        ForwardingTable::RetainOpenPublicationAfterCopy(state.region, reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_FALSE(static_cast<bool>(late));

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(FindToPublicState, NotManagedIsObservable)
{
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, nullptr);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::NotManaged);
    GC_EXPECT_TRUE(result.found() == nullptr);
}

GC_OTHER_VM_TEST(FindToPublicState, QueryableMissIsObservable)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = fx.region0;
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(0));
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, from);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::NotForwarded);
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(1));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(0));

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_OTHER_VM_TEST(FindToPublicState, UnavailableIsObservable)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = fx.region0;
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    // Table-gone + Usable is NotForwarded (zGeneration.inline.hpp:131-140).
    // FORWARDED keeps the fail-closed Unavailable exit.
    from->SetStateCode(ObjectState::FORWARDED);

    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(0));
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, from);
    // PrepareForwardable armed a table but never InsertMapping. ReclaimRetired
    // may destroy that empty carrier. Unavailable is the never-selected /
    // never-installed object, not a compact-receipt lifetime hole.
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(result.unavailable_lookup_publication_closed());
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(1));

    from->SetStateCode(ObjectState::NORMAL);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// A single product-linked construction exercises two distinct Unavailable producers.  It proves
// the route witness is not a constant formatter: one arm closes an installed publication while
// keeping its ghost region, and the other uses an unarmed, non-ghost region with a FORWARDED
// header. Both answers come from WCollector::FindToVersion in libcangjie-runtime.so.
GC_OTHER_VM_TEST(FindToRouteDiagnostics, DistinguishesLookupUnavailableFromNoGhostForwarded)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);

    RegionInfo* lookupRegion = fx.region0;
    BaseObject* lookupFrom = fx.PlaceObject(lookupRegion->GetRegionStart() + 64);
    lookupRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    lookupRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(lookupFrom) + lookupFrom->GetSize());
    LiveInfo* live = PrepareForwardable(fx, lookupRegion, reinterpret_cast<MAddress>(lookupFrom));
    ForwardingTable::ClearEntries(lookupRegion->GetRegionStart(), lookupRegion->GetRegionSize());
    DestroyAfterGhostCleared(lookupRegion, "gc-unit-explicit-coverage");
    lookupFrom->SetStateCode(ObjectState::FORWARDED);

    FindToVersionResult lookup =
        RelocationReceiptTestAccess::ProductFindToVersion(collector, lookupFrom);
    GC_EXPECT_TRUE(lookup.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(lookup.unavailable_route() ==
                   FindToVersionResult::UnavailableRoute::LookupUnavailable);
    GC_EXPECT_FALSE(lookup.unavailable_forwarded_valid());
    GC_EXPECT_FALSE(lookup.unavailable_forwarded());
    GC_EXPECT_FALSE(lookup.unavailable_from_region_info_null_valid());
    GC_EXPECT_FALSE(lookup.unavailable_from_region_info_null());
    GC_EXPECT_TRUE(std::strcmp(lookup.unavailable_lookup_answer(), "unavailable") == 0);
    GC_EXPECT_TRUE(lookup.unavailable_lookup_snapshot_valid());
    GC_EXPECT_TRUE(std::strcmp(lookup.unavailable_lookup_cause(),
                               "publication_closed+table_destroyed") == 0);
    GC_EXPECT_FALSE(lookup.unavailable_lookup_active_candidate());
    GC_EXPECT_TRUE(std::strcmp(lookup.unavailable_lookup_active_answer(), "unarmed") == 0);
    GC_EXPECT_TRUE(std::strcmp(lookup.unavailable_lookup_retired_answer(), "unarmed") == 0);
    GC_EXPECT_TRUE(lookup.unavailable_lookup_publication_closed());
    GC_EXPECT_TRUE(std::strcmp(lookup.unavailable_route_name(), "lookup_unavailable") == 0);

    RegionInfo* noGhostRegion = fx.region1;
    BaseObject* noGhostFrom = fx.PlaceObject(noGhostRegion->GetRegionStart() + 64);
    noGhostRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    noGhostRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(noGhostFrom) + noGhostFrom->GetSize());
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(noGhostFrom)) == nullptr);
    GC_EXPECT_FALSE(ForwardingTable::EntriesArmed(reinterpret_cast<MAddress>(noGhostFrom)));
    noGhostFrom->SetStateCode(ObjectState::FORWARDED);

    FindToVersionResult noGhost =
        RelocationReceiptTestAccess::ProductFindToVersion(collector, noGhostFrom);
    GC_EXPECT_TRUE(noGhost.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(noGhost.unavailable_route() ==
                   FindToVersionResult::UnavailableRoute::NoGhostForwarded);
    GC_EXPECT_TRUE(noGhost.unavailable_forwarded_valid());
    GC_EXPECT_TRUE(noGhost.unavailable_forwarded());
    GC_EXPECT_TRUE(noGhost.unavailable_from_region_info_null_valid());
    GC_EXPECT_TRUE(noGhost.unavailable_from_region_info_null());
    GC_EXPECT_TRUE(std::strcmp(noGhost.unavailable_lookup_answer(), "unarmed") == 0);
    GC_EXPECT_TRUE(noGhost.unavailable_lookup_snapshot_valid());
    GC_EXPECT_TRUE(std::strcmp(noGhost.unavailable_lookup_cause(), "none") == 0);
    GC_EXPECT_FALSE(noGhost.unavailable_lookup_active_candidate());
    GC_EXPECT_TRUE(std::strcmp(noGhost.unavailable_lookup_active_answer(), "unarmed") == 0);
    GC_EXPECT_TRUE(std::strcmp(noGhost.unavailable_lookup_retired_answer(), "unarmed") == 0);
    GC_EXPECT_FALSE(noGhost.unavailable_lookup_publication_closed());
    GC_EXPECT_TRUE(std::strcmp(noGhost.unavailable_route_name(), "no_ghost_forwarded") == 0);
    GC_EXPECT_NE(lookup.unavailable_route(), noGhost.unavailable_route());

    noGhostFrom->SetStateCode(ObjectState::NORMAL);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    lookupRegion->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// LookupTo returns the decision record itself.  Change both metadata faces only
// after the product lookup returns, then prove the record still describes the
// carrier inputs that selected Unavailable rather than those later faces.
GC_OTHER_VM_TEST(LookupDecisionSnapshot, SurvivesPostReturnGhostAndHeaderMutation)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = fx.region0;
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(from)) == region);
    GC_EXPECT_FALSE(from->IsForwarded());

    const ForwardingTable::LookupResult result =
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    region->DispelGhostFromRegion();
    from->SetStateCode(ObjectState::FORWARDED);
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");

    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(from)) == nullptr);
    GC_EXPECT_TRUE(result.answer == ForwardingTable::ToAnswer::Unavailable);
    GC_EXPECT_TRUE((static_cast<uint8_t>(result.unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::PublicationClosed)) != 0);
    GC_EXPECT_TRUE((static_cast<uint8_t>(result.unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::NeverInstalled)) != 0);
    GC_EXPECT_FALSE(result.activeCandidate);
    GC_EXPECT_TRUE(result.activeAnswer == ForwardingTable::ToAnswer::Unarmed);
    GC_EXPECT_TRUE(result.retiredAnswer == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_TRUE(result.publicationClosed);
    GC_EXPECT_TRUE(result.forwardingSnapshotValid);

    from->SetStateCode(ObjectState::NORMAL);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

#if defined(MRT_TESTABLE_INTERNALS) && defined(MRT_FINDTO_RETAIN_TEST)
struct RetainWindowState {
    std::mutex mutex;
    std::condition_variable cv;
    bool lookupRetained = false;
    bool releaseLookup = false;
    bool clearStarted = false;
    bool clearDone = false;
};

void HoldRetainedLookup(void* context)
{
    auto& state = *static_cast<RetainWindowState*>(context);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.lookupRetained = true;
    state.cv.notify_all();
    state.cv.wait(lock, [&state]() { return state.releaseLookup; });
}

// The hook setter is a testability export that only exists when the product SO
// itself was compiled with MRT_TESTABLE_INTERNALS. Bind it at runtime (same
// pattern as test_live_map.cpp) so this TU keeps linking against the default
// OFF product, where the guarded block below is compiled out anyway.
using ProductSetLookupRetainHook = void (*)(void (*)(void*), void*);

static ProductSetLookupRetainHook ProductSetLookupRetainHookFn()
{
    void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        handle = dlopen("libcangjie-runtime.so", RTLD_NOW);
    }
    GC_EXPECT_TRUE(handle != nullptr);
    auto fn = reinterpret_cast<ProductSetLookupRetainHook>(
        dlsym(handle, "_ZN12MapleRuntime15ForwardingTable19SetLookupRetainHookEPFvPvES1_"));
    // This test is the positive retain-window arm.  A product built without
    // the test hook is not a passing observation; it is a missing precondition.
    GC_EXPECT_TRUE(fn != nullptr);
    return fn;
}

GC_OTHER_VM_TEST(FindToRetainWindow, ActiveLookupPinsCarrierUntilQueryReturns)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = fx.region0;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    ProductSetLookupRetainHook setHook = ProductSetLookupRetainHookFn();
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    RetainWindowState state;
    setHook(HoldRetainedLookup, &state);
    FindToVersionResult queryResult = FindToVersionResult::NotManaged();

    std::thread query([&]() {
        queryResult = RelocationReceiptTestAccess::ProductFindToVersion(collector, from);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        // Bounded: if the product never pins the carrier (retain pin cut), the
        // hook never fires and this must fail here, not hang.
        const bool pinned = state.cv.wait_for(lock, std::chrono::seconds(10),
                                              [&state]() { return state.lookupRetained; });
        GC_EXPECT_TRUE(pinned);
    }
    std::thread clear([&]() {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.clearStarted = true;
            state.cv.notify_all();
        }
        ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.clearDone = true;
            state.cv.notify_all();
        }
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&state]() { return state.clearStarted; });
        GC_EXPECT_FALSE(state.cv.wait_for(
            lock, std::chrono::milliseconds(100), [&state]() { return state.clearDone; }));
        state.releaseLookup = true;
        state.cv.notify_all();
    }
    query.join();
    clear.join();
    setHook(nullptr, nullptr);
    // A carrier that was present in the active slot but refused retain is a
    // lifecycle failure, not an ordinary armed miss (ForwardingTable.cpp:896).
    GC_EXPECT_TRUE(queryResult.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(state.clearDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
#endif

GC_TEST(ForwardingPublicationProduct, ClearEntriesRetiresAndDropsWholeSpan)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(1, 2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    ZForwarding* table = ForwardingTable::GetEntries(region->GetRegionStart());
    GC_EXPECT_TRUE(table != nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart() + RegionInfo::UNIT_SIZE) == table);

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart()) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart() + RegionInfo::UNIT_SIZE) == nullptr);
    ForwardingTable::Publication lateBeforeCopy =
        ForwardingTable::EnsurePublicationBeforeCopy(region, region->GetRegionStart());
    GC_EXPECT_FALSE(static_cast<bool>(lateBeforeCopy));
    GC_EXPECT_FALSE(ForwardingTable::InsertProvisional(
        region->GetRegionStart(), region->GetRegionSize(), region));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart()) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize()));

    DestroyAfterGhostCleared(region, "gc-unit-explicit-coverage");
    GC_EXPECT_FALSE(ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize()));
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, KeptInPlacePublishesIdentityBeforeRetire)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    region->MarkForwardingDone();

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);

    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(from));
    RefField<> qualified = RelocationReceiptTestAccess::QualifyStoreValue(collector, from);
    GC_EXPECT_EQ(raw(qualified.GetTargetObject()), reinterpret_cast<MAddress>(from));

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, KeptActiveReceiptRemainsRequiredAfterTableRetires)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(publication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(to)).address,
                 reinterpret_cast<MAddress>(to));
    publication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-kept-active-receipt");
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_TRUE(RelocationReceiptTestAccess::ProductFindToVersion(collector, from).found() == to);

    from->SetStateCode(ObjectState::NORMAL);
    DestroyAfterGhostCleared(region, "gc-unit-kept-active-receipt-cleanup");
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)), 0);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    RelocationReceiptTestAccess::ReleaseListOwnership(destination);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, KeptInPlaceLivemapStartsSurviveOverwrittenPrefix)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* first = fx.PlaceObject(region->GetRegionStart());
    BaseObject* second = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(first));
    RegionBitmap* bitmap = live->GetMarkFace().bitmap;
    GC_EXPECT_TRUE(bitmap != nullptr);
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(second)),
                           second->GetSize(), region->GetRegionSize());
    region->RecordRouteStart(region->GetAddressOffset(reinterpret_cast<MAddress>(second)));
    region->AddLiveByteCount(second->GetSize());
    *reinterpret_cast<uint64_t*>(first) = 0;
    region->MarkForwardingDone();

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);

    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(second)),
                 reinterpret_cast<MAddress>(second));

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// zRelocationSet.cpp:91-96 and zRelocate.cpp:1013-1047: retiring the old
// forwarding generation and installing the next one must not leave an object
// header claiming FORWARDED after the receipt that justified it is gone.
GC_TEST(ForwardingPublicationProduct, PrepareForwardableClearsNormalRouteResidualHeader)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(5));
    RegionInfo* region = RegionInfo::InitRegion(5, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(
        publication, reinterpret_cast<MAddress>(from), reinterpret_cast<MAddress>(to));
    GC_EXPECT_EQ(receipt.address, reinterpret_cast<MAddress>(to));
    publication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();

    region->DispelGhostFromRegion();
    GC_EXPECT_TRUE(region->GetRouteState() == RegionInfo::RouteState::NORMAL);
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));

    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    GC_EXPECT_FALSE(from->IsForwarded());
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(reinterpret_cast<MAddress>(from)));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)), 0);

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-normal-route-residual");
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Second family-8 path: a ROUTED page can retain a prior from->to receipt after
// raw-pin clears ghost and the next generation installs an empty active table.
// Retirement must preserve that receipt; active miss is not identity evidence.
GC_TEST(ForwardingPublicationProduct, ExemptPreservesRetiredReceiptAcrossActiveGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    ForwardingTable::Publication oldPublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(oldPublication));
    const ZForwarding::Receipt oldReceipt = ForwardingTable::InstallMapping(
        oldPublication, reinterpret_cast<MAddress>(from), reinterpret_cast<MAddress>(to));
    GC_EXPECT_EQ(oldReceipt.address, reinterpret_cast<MAddress>(to));
    oldPublication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_TRUE(region->IsGhostFromRegion());

    // POST_TRACE raw-pin clears ghost without normalizing ROUTED, then the
    // next generation installs an empty active table for the same range.
    region->ClearGhostRegionBit();
    GC_EXPECT_FALSE(region->IsGhostFromRegion());
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());

    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)), 0);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);

    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)), 0);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    BaseObject* consumer = RelocationReceiptTestAccess::ProductFindToVersion(collector, from).found();
    std::fprintf(stderr,
                 "MUTUALWAIT_DETAIL from=%p expected_to=%p active=%p retired=%p consumer=%p\n",
                 from, to,
                 reinterpret_cast<void*>(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from))),
                 reinterpret_cast<void*>(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from))),
                 consumer);
    GC_EXPECT_TRUE(consumer == to);

    from->SetStateCode(ObjectState::NORMAL);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    RelocationReceiptTestAccess::ReleaseListOwnership(destination);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// zGeneration.cpp:276-285 resets the old relocation set only after remap has
// consumed every source reference. A residual FORWARDED source proves that the
// port has not reached that state: keep its old receipt until a newer active
// generation publishes a successor instead of inferring identity at retirement.
GC_TEST(ForwardingPublicationProduct, ReclaimRetiredDefersResidualUntilActiveReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    ForwardingTable::Publication oldPublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(oldPublication));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(oldPublication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(to)).address,
                 reinterpret_cast<MAddress>(to));
    oldPublication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();
    region->ClearGhostRegionBit();
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());

    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)), 0);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);
    ForwardingTable::ReclaimRetired("gc-unit-last-receipt");
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_TRUE(RelocationReceiptTestAccess::ProductFindToVersion(collector, from).found() == to);

    ForwardingTable::Publication activePublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(activePublication));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(activePublication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(to)).address,
                 reinterpret_cast<MAddress>(to));
    activePublication = ForwardingTable::Publication();
    ForwardingTable::ReclaimRetired("gc-unit-active-receipt");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_TRUE(from->IsForwarded());

    from->SetStateCode(ObjectState::NORMAL);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-last-receipt-cleanup");
    RelocationReceiptTestAccess::ReleaseListOwnership(destination);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ReclaimRetiredPreservesNewActiveReceiptHeader)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(2));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* oldDestination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* newDestination = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && oldDestination != nullptr && newDestination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    oldDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    newDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* oldTo = fx.PlaceObject(oldDestination->GetRegionStart());
    BaseObject* newTo = fx.PlaceObject(newDestination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    oldDestination->SetRegionAllocPtr(reinterpret_cast<MAddress>(oldTo) + oldTo->GetSize());
    newDestination->SetRegionAllocPtr(reinterpret_cast<MAddress>(newTo) + newTo->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    ForwardingTable::Publication oldPublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(oldPublication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(oldTo)).address,
                 reinterpret_cast<MAddress>(oldTo));
    oldPublication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();
    region->ClearGhostRegionBit();
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    ForwardingTable::Publication activePublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(activePublication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(newTo)).address,
                 reinterpret_cast<MAddress>(newTo));
    activePublication = ForwardingTable::Publication();

    ForwardingTable::ReclaimRetired("gc-unit-active-receipt");
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(oldTo));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(newTo));
    GC_EXPECT_TRUE(RelocationReceiptTestAccess::ProductFindToVersion(collector, from).found() == newTo);

    from->SetStateCode(ObjectState::NORMAL);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-active-receipt-cleanup");
    RelocationReceiptTestAccess::ReleaseListOwnership(oldDestination);
    RelocationReceiptTestAccess::ReleaseListOwnership(newDestination);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, FinishIncompleteUnmovablePublishesIdentityBeforeDone)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* survivor = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(survivor) + survivor->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(survivor));

    RegionManager manager;
    manager.ParkUnmovableFromRegion(region);
    manager.FinishIncompleteFromRegions();

    GC_EXPECT_TRUE(region->IsForwardingDone());
    BaseObject* consumer = RelocationReceiptTestAccess::ProductFindToVersion(collector, survivor).found();
    GC_EXPECT_TRUE(consumer == survivor);
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(survivor)),
                 reinterpret_cast<MAddress>(survivor));

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-finish-unmovable");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, FinishIncompleteNonFromResidualPublishesIdentityBeforeDone)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* survivor = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(survivor) + survivor->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(survivor));

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    manager.FinishIncompleteFromRegions();

    GC_EXPECT_TRUE(region->IsForwardingDone());
    BaseObject* consumer = RelocationReceiptTestAccess::ProductFindToVersion(collector, survivor).found();
    GC_EXPECT_TRUE(consumer == survivor);
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(survivor)),
                 reinterpret_cast<MAddress>(survivor));

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-finish-nonfrom");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ResolveStoreValueSafeAddrAfterForwardingTableGone)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    DestroyAfterGhostCleared(region, "gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(liveObject)) == nullptr);
    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, liveObject);
    GC_EXPECT_TRUE(resolved == liveObject);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, CompactRegionDeadFromHasNoForwardingAndIsNotTlab)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    BaseObject* deadObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(deadObject) + deadObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());

    const MAddress deadAddr = reinterpret_cast<MAddress>(deadObject);
    const MAddress liveAddr = reinterpret_cast<MAddress>(liveObject);
    GC_EXPECT_EQ(ForwardingTable::FindTo(deadAddr), static_cast<MAddress>(0));
    GC_EXPECT_TRUE(ForwardingTable::FindTo(liveAddr) != static_cast<MAddress>(0));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart()) != nullptr);
    GC_EXPECT_TRUE(region->GetRegionType() != RegionInfo::RegionType::THREAD_LOCAL_REGION);
    GC_EXPECT_TRUE(region->GetRegionType() == RegionInfo::RegionType::RECENT_FULL_REGION);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

#if defined(__linux__)
template <typename Fn>
void ExpectRootAbortAt(const char* siteSubstr, Fn&& fn)
{
    int pipefd[2];
    GC_EXPECT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        fn();
        _exit(0);
    }
    close(pipefd[1]);
    char buf[8192];
    size_t filled = 0;
    while (filled + 1 < sizeof(buf)) {
        const ssize_t n = read(pipefd[0], buf + filled, sizeof(buf) - 1 - filled);
        if (n <= 0) {
            break;
        }
        filled += static_cast<size_t>(n);
    }
    buf[filled] = '\0';
    close(pipefd[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    if (std::strstr(buf, siteSubstr) == nullptr) {
        std::fprintf(stderr, "ABORT_CAPTURE filled=%zu needle=%s\n---\n%s\n---\n",
                     filled, siteSubstr, buf);
    }
    GC_EXPECT_TRUE(std::strstr(buf, siteSubstr) != nullptr);
}

template <typename Fn>
void ExpectRootAbort(Fn&& fn)
{
    ExpectRootAbortAt("[LOADFC][fail-closed]", fn);
}

struct AbortCapture {
    int status;
    std::string output;
};

template <typename Fn>
AbortCapture CaptureAbort(Fn&& fn)
{
    int pipefd[2];
    GC_EXPECT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        fn();
        _exit(0);
    }
    close(pipefd[1]);
    std::string output;
    char buffer[1024];
    for (;;) {
        const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        GC_EXPECT_EQ(errno, EINTR);
    }
    close(pipefd[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    return AbortCapture{ status, std::move(output) };
}

template <typename BeforeLookup>
AbortCapture CaptureNeverInstalledAbort(WCollector& collector, BaseObject* target, BeforeLookup&& beforeLookup)
{
    return CaptureAbort([&]() {
        beforeLookup();
        FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, target);
        RootSlot slot;
        StorePlain(slot, from_object(target));
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, target, &slot };
        (void)result.GetOrFailClosed("NeverInstalledDiagnostic.fixture", provenance);
    });
}

// The receipt, retirement and lookup all belong to the linked product SO.
// Save the expected identity from the actual publisher before retiring it;
// no LookupResult is constructed or passed to a product consumer by this test.
struct LookupWitnessIdentity {
    uintptr_t tableId;
    MAddress start;
    uint64_t generation;
    uint64_t epoch;
    RegionLifeId lifeId;
};

LookupWitnessIdentity ReadLookupWitnessIdentity(ZForwarding* table)
{
    GC_EXPECT_TRUE(table != nullptr);
    const ZForwarding::FromPageView* view = table->from_page_snapshot();
    GC_EXPECT_TRUE(view != nullptr);
    return { reinterpret_cast<uintptr_t>(table), table->start(), table->publication_generation(),
             view->epoch, view->lifeId };
}

void ExpectDiagnosticLookupIdentity(const std::string& output, const LookupWitnessIdentity& expected)
{
    char table[64] {};
    (void)std::snprintf(table, sizeof(table), "table_id=%#zx ", static_cast<size_t>(expected.tableId));
    const std::string generation = "publication_generation=" + std::to_string(expected.generation) + " ";
    const std::string epoch = "from_page_epoch=" + std::to_string(expected.epoch) + " ";
    const std::string life = "lifeId=" + std::to_string(expected.lifeId) + " ";
    const bool tableIdentityMatches = output.find(table) != std::string::npos;
    const bool publicationGenerationMatches = output.find(generation) != std::string::npos;
    const bool fromPageEpochMatches = output.find(epoch) != std::string::npos;
    const bool fromPageLifeIdMatches = output.find(life) != std::string::npos;
    // Print every comparison before a throwing assertion: a field-specific
    // product cut must change only its corresponding result in this record.
    std::fprintf(stderr, "LOOKUP_WITNESS_TARGET diagnostic table=%d generation=%d epoch=%d life=%d\n",
                 tableIdentityMatches, publicationGenerationMatches, fromPageEpochMatches, fromPageLifeIdMatches);
    GC_EXPECT_TRUE(tableIdentityMatches);
    GC_EXPECT_TRUE(publicationGenerationMatches);
    GC_EXPECT_TRUE(fromPageEpochMatches);
    GC_EXPECT_TRUE(fromPageLifeIdMatches);
}

enum class LookupWitnessConsumer { Lookup, LoadDiagnostic, StoreDiagnostic, StoreResolutionDiagnostic };

void CheckLookupWitness(bool retirePublisher, bool addCandidate, bool retireCandidate,
                        bool publishReceipt, LookupWitnessConsumer consumer = LookupWitnessConsumer::Lookup)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    const MAddress from = reinterpret_cast<MAddress>(state.from);
    const MAddress to = reinterpret_cast<MAddress>(state.to);
    const LookupWitnessIdentity publisher = ReadLookupWitnessIdentity(ForwardingTable::GetEntries(from));
    if (publishReceipt) {
        ForwardingTable::Publication publication =
            ForwardingTable::EnsurePublicationBeforeCopy(state.region, from);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InstallMapping(publication, from, to).address, to);
    }
    if (retirePublisher) {
        ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    }
    LookupWitnessIdentity expected = publisher;
    if (addCandidate) {
        GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
            state.region->GetRegionStart(), state.region->GetRegionSize()));
        GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
            state.region->GetRegionStart(), state.region->GetRegionSize(), state.region));
        GC_EXPECT_TRUE(ForwardingTable::PublishFromPageView(
            state.region, state.live, publisher.epoch + 17,
            state.region->GetRegionAllocPtr(), state.region->GetMarkStartAllocPtr(),
            state.region->GetLiveByteCount(), 1, 0, state.region->GetRegionLifeId()));
        const LookupWitnessIdentity candidate = ReadLookupWitnessIdentity(ForwardingTable::GetEntries(from));
        GC_EXPECT_NE(candidate.tableId, publisher.tableId);
        GC_EXPECT_NE(candidate.generation, publisher.generation);
        GC_EXPECT_NE(candidate.epoch, publisher.epoch);
        if (!publishReceipt) {
            expected = candidate;
        }
        if (retireCandidate) {
            ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
        }
    }

    if (consumer != LookupWitnessConsumer::Lookup) {
        // Enter the existing last-chance load diagnostic. It performs its own
        // LookupTo and consumes that result; the expected tuple is only used
        // by the parent to check the product's emitted record.
        const AbortCapture aborted = CaptureAbort([&]() {
            if (consumer == LookupWitnessConsumer::StoreResolutionDiagnostic) {
                // The destination no longer has an object header, but this
                // retired receipt still names its current page life. Enter the
                // mutator store barrier and inspect ResolveStoreValue's own
                // diagnostic lookup, before its final FailClosedLoad record.
                *reinterpret_cast<uintptr_t*>(state.to) = 0;
                state.region->DispelGhostFromRegion();
                BaseObject* holder = fx.obj0;
                auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
                Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
                barrier.WriteReference(holder, field, state.from);
                return;
            }
            if (consumer == LookupWitnessConsumer::StoreDiagnostic) {
                RelocationReceiptTestAccess::CheckStoreGoodTarget(collector, state.from);
                return;
            }
            Collector::FailClosedLoad("ForwardingLookupWitness", state.from, from,
                ForwardingProvenance{ ForwardingHolderKind::HeapRef, state.from, &state.from });
        });
        std::fprintf(stderr, "LOOKUP_WITNESS_DIAGNOSTIC status=%d\n%s", aborted.status, aborted.output.c_str());
        GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
        GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
        const char* entry = consumer == LookupWitnessConsumer::StoreResolutionDiagnostic
            ? "[FWDTABLE][resolve-miss] site=no-forwarding"
            : (consumer == LookupWitnessConsumer::LoadDiagnostic
                ? "[LOADFC][fail-closed] site=ForwardingLookupWitness" : "consumer=ForwardingLookupWitness");
        const size_t begin = aborted.output.find(entry);
        GC_EXPECT_TRUE(begin != std::string::npos);
        const size_t end = aborted.output.find('\n', begin);
        const std::string record = aborted.output.substr(begin, end - begin);
        const std::string hit = std::to_string(static_cast<unsigned>(ForwardingTable::ToAnswer::ArmedHit));
        GC_EXPECT_TRUE(record.find("lookup_state=" + hit + " ") != std::string::npos);
        if (consumer != LookupWitnessConsumer::StoreDiagnostic) {
            GC_EXPECT_TRUE(record.find("retired_lookup=" + hit + " ") != std::string::npos);
        }
        ExpectDiagnosticLookupIdentity(record, expected);
    } else {
        const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
        std::fprintf(stderr,
            "LOOKUP_WITNESS_TARGET to=%#zx table=%#zx generation=%llu epoch=%llu expected_table=%#zx\n",
            static_cast<size_t>(lookup.to), static_cast<size_t>(lookup.tableId),
            static_cast<unsigned long long>(lookup.publicationGeneration),
            static_cast<unsigned long long>(lookup.fromPageEpoch), static_cast<size_t>(expected.tableId));
        GC_EXPECT_EQ(lookup.to, publishReceipt ? to : 0);
        GC_EXPECT_TRUE(lookup.answer == (publishReceipt ? ForwardingTable::ToAnswer::ArmedHit :
            (retireCandidate ? ForwardingTable::ToAnswer::Unavailable : ForwardingTable::ToAnswer::ArmedMiss)));
        GC_EXPECT_EQ(lookup.tableId, expected.tableId);
        GC_EXPECT_EQ(lookup.carrierStart, expected.start);
        GC_EXPECT_EQ(lookup.publicationGeneration, expected.generation);
        GC_EXPECT_EQ(lookup.fromPageEpoch, expected.epoch);
        GC_EXPECT_EQ(lookup.fromPageLifeId, expected.lifeId);
        GC_EXPECT_TRUE(lookup.forwardingSnapshotValid);
    }
    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, ActiveHitIdentifiesPublisher)
{
    CheckLookupWitness(false, false, false, true);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, RetiredHitReplacesActiveMissIdentity)
{
    CheckLookupWitness(true, true, false, true);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, LaterRetiredHitReplacesFirstCoverIdentity)
{
    CheckLookupWitness(true, true, true, true);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, ActiveMissKeepsCandidateIdentity)
{
    CheckLookupWitness(true, true, false, false);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, RetiredMissKeepsFirstCoverIdentity)
{
    CheckLookupWitness(true, true, true, false);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, LoadDiagnosticConsumesRetiredHitAfterActiveMiss)
{
    CheckLookupWitness(true, true, false, true, LookupWitnessConsumer::LoadDiagnostic);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, LoadDiagnosticConsumesLaterRetiredHit)
{
    CheckLookupWitness(true, true, true, true, LookupWitnessConsumer::LoadDiagnostic);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, StoreDiagnosticConsumesLaterRetiredHit)
{
    CheckLookupWitness(true, true, true, true, LookupWitnessConsumer::StoreDiagnostic);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, StoreBarrierDiagnosticConsumesLaterRetiredHit)
{
    CheckLookupWitness(true, true, true, true, LookupWitnessConsumer::StoreResolutionDiagnostic);
}

RefField<>* gIncomingDestination = nullptr;
uintptr_t gIncomingDestinationExpected = 0;

void IncomingAbortWitness(int)
{
    const uintptr_t observed = gIncomingDestination == nullptr
        ? static_cast<uintptr_t>(-1)
        : raw(gIncomingDestination->GetFieldValue());
    static constexpr char kUnchanged[] = "INCOMING_DESTINATION_UNCHANGED\n";
    static constexpr char kChanged[] = "INCOMING_DESTINATION_CHANGED\n";
    if (observed == gIncomingDestinationExpected) {
        (void)write(STDERR_FILENO, kUnchanged, sizeof(kUnchanged) - 1);
        _exit(86);
    }
    (void)write(STDERR_FILENO, kChanged, sizeof(kChanged) - 1);
    _exit(87);
}

uintptr_t OneLoadBadRemap()
{
    const uintptr_t bad = static_cast<uintptr_t>(::g_cjLoadBadMask) & REMAP_COLOUR_MASK;
    GC_EXPECT_TRUE(bad != 0);
    return bad & (~bad + 1);
}

GC_OTHER_VM_TEST(NeverInstalledDiagnostic, NeverInstalledListsAllCoveringCarriers)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_DIAG", "neverinstalled", 1), 0);
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    AbortCapture single = CaptureNeverInstalledAbort(collector, state.from, []() {});
    GC_EXPECT_TRUE(WIFSIGNALED(single.status));
    GC_EXPECT_EQ(WTERMSIG(single.status), SIGABRT);
    GC_EXPECT_TRUE(single.output.find("[FINDTO][never-installed]") != std::string::npos);
    GC_EXPECT_TRUE(single.output.find("covering_total=1 covering_emitted=1") != std::string::npos);
    GC_EXPECT_EQ(CountSubstring(single.output, "table_generation="), static_cast<size_t>(1));
    GC_EXPECT_TRUE(single.output.find("state=retired,answer=armed_miss") != std::string::npos);
    GC_EXPECT_TRUE(single.output.find("pending_destroy=") != std::string::npos);
    GC_EXPECT_TRUE(single.output.find("carrier_overflow=0") != std::string::npos);
    GC_EXPECT_TRUE(single.output.find("never_installed_event=1") != std::string::npos);

    const uint64_t secondGeneration = RetireAnotherEmptyCarrier(state);
    AbortCapture pair = CaptureNeverInstalledAbort(collector, state.from, []() {});
    GC_EXPECT_TRUE(WIFSIGNALED(pair.status));
    GC_EXPECT_EQ(WTERMSIG(pair.status), SIGABRT);
    GC_EXPECT_TRUE(pair.output.find("covering_total=2 covering_emitted=2") != std::string::npos);
    GC_EXPECT_EQ(CountSubstring(pair.output, "table_generation="), static_cast<size_t>(2));
    GC_EXPECT_EQ(CountSubstring(pair.output, "answer=armed_miss"), static_cast<size_t>(2));
    GC_EXPECT_TRUE(pair.output.find("publication_generation=" + std::to_string(state.generation)) !=
                   std::string::npos);
    GC_EXPECT_TRUE(pair.output.find("publication_generation=" + std::to_string(secondGeneration)) !=
                   std::string::npos);
    GC_EXPECT_TRUE(pair.output.find("carrier_overflow=0") != std::string::npos);

    // Positive control for the state-machine assertion: manufacture the state
    // product ClearEntries makes unreachable (closed publication + active
    // carrier). Default product SOs deliberately omit this test-only export;
    // the test configuration below requires and executes it.
    ProductForcePublicationClosedForTest forceClosed = ProductForcePublicationClosedForTestFn();
#if defined(MRT_FINDTO_RETAIN_TEST)
    GC_EXPECT_TRUE(forceClosed != nullptr);
#endif
    if (forceClosed != nullptr) {
        GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
            state.region->GetRegionStart(), state.region->GetRegionSize()));
        GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
            state.region->GetRegionStart(), state.region->GetRegionSize(), state.region));
        GC_EXPECT_TRUE(ForwardingTable::PublishFromPageView(
            state.region, state.live, state.region->GetSnapshotEpoch(),
            state.region->GetRegionAllocPtr(), state.region->GetMarkStartAllocPtr(),
            state.region->GetLiveByteCount(), 1, 0, state.region->GetRegionLifeId()));
        AbortCapture impossibleActive = CaptureNeverInstalledAbort(collector, state.from, [&]() {
            forceClosed(reinterpret_cast<MAddress>(state.from));
        });
        GC_EXPECT_TRUE(WIFSIGNALED(impossibleActive.status));
        GC_EXPECT_EQ(WTERMSIG(impossibleActive.status), SIGABRT);
        GC_EXPECT_TRUE(impossibleActive.output.find("state=active_closed") != std::string::npos);
        GC_EXPECT_TRUE(impossibleActive.output.find("state_machine_violation=1") != std::string::npos);
        GC_EXPECT_TRUE(impossibleActive.output.find("[FINDTO][never-installed-state]") != std::string::npos);
    }

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(NeverInstalledDiagnostic, NeverInstalledCurrentIncarnationDelta)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_DIAG", "neverinstalled", 1), 0);
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    AbortCapture sameLife = CaptureNeverInstalledAbort(collector, state.from, [&]() {
        state.region->BumpSnapshotEpoch();
    });
    GC_EXPECT_TRUE(WIFSIGNALED(sameLife.status));
    GC_EXPECT_EQ(WTERMSIG(sameLife.status), SIGABRT);
    GC_EXPECT_TRUE(sameLife.output.find("witness_epoch_delta=1") != std::string::npos);
    GC_EXPECT_TRUE(sameLife.output.find("witness_epoch_delta=n/a(reused)") == std::string::npos);

    AbortCapture reused = CaptureNeverInstalledAbort(collector, state.from, [&]() {
        // InitRegionInfo's incarnation edge is BumpRegionLifeId.  The mutation
        // is isolated in this fork because product reuse correctly refuses to
        // pass a live retired carrier.
        state.region->BumpRegionLifeId();
    });
    GC_EXPECT_TRUE(WIFSIGNALED(reused.status));
    GC_EXPECT_EQ(WTERMSIG(reused.status), SIGABRT);
    GC_EXPECT_TRUE(reused.output.find("witness_epoch_delta=n/a(reused)") != std::string::npos);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(NeverInstalledDiagnostic, NeverInstalledRawHeaderVerdict)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_DIAG", "neverinstalled", 1), 0);
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    AbortCapture forwarded = CaptureNeverInstalledAbort(collector, state.from, [&]() {
        state.from->SetStateCode(ObjectState::FORWARDED);
    });
    GC_EXPECT_TRUE(WIFSIGNALED(forwarded.status));
    GC_EXPECT_EQ(WTERMSIG(forwarded.status), SIGABRT);
    GC_EXPECT_TRUE(forwarded.output.find("hand_verdict=Forwarded") != std::string::npos);

    AbortCapture zero = CaptureNeverInstalledAbort(collector, state.from, [&]() {
        *reinterpret_cast<uint64_t*>(state.from) = 0;
    });
    GC_EXPECT_TRUE(WIFSIGNALED(zero.status));
    GC_EXPECT_EQ(WTERMSIG(zero.status), SIGABRT);
    GC_EXPECT_TRUE(zero.output.find("raw_target_header=0 hand_verdict=ZeroHeader") != std::string::npos);

    AbortCapture usable = CaptureNeverInstalledAbort(collector, state.from, [&]() {
        (void)fx.PlaceObject(reinterpret_cast<MAddress>(state.from));
    });
    GC_EXPECT_TRUE(WIFSIGNALED(usable.status));
    GC_EXPECT_EQ(WTERMSIG(usable.status), SIGABRT);
    GC_EXPECT_TRUE(usable.output.find("hand_verdict=Usable") != std::string::npos);
    GC_EXPECT_TRUE(usable.output.find("reverse_total=0") != std::string::npos);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    // Header-only classification calls both an ordinary object and an
    // already-remapped destination Usable.  The cold reverse receipt scan is
    // the conditional fourth diagnostic which distinguishes the latter.
    LateBackfillState reverse = PrepareLateBackfill(fx, collector);
    LiveInfo* destinationLive = PrepareForwardable(
        fx, reverse.destination, reinterpret_cast<MAddress>(reverse.to));
    {
        ForwardingTable::Publication publication = ForwardingTable::RetainOpenPublicationAfterCopy(
            reverse.region, reinterpret_cast<MAddress>(reverse.from));
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                         publication, reinterpret_cast<MAddress>(reverse.from),
                         reinterpret_cast<MAddress>(reverse.to)),
                     reinterpret_cast<MAddress>(reverse.to));
    }
    ForwardingTable::ClearEntries(reverse.region->GetRegionStart(), reverse.region->GetRegionSize());
    ForwardingTable::ClearEntries(
        reverse.destination->GetRegionStart(), reverse.destination->GetRegionSize());

    AbortCapture alreadyTo = CaptureNeverInstalledAbort(collector, reverse.to, []() {});
    GC_EXPECT_TRUE(WIFSIGNALED(alreadyTo.status));
    GC_EXPECT_EQ(WTERMSIG(alreadyTo.status), SIGABRT);
    GC_EXPECT_TRUE(alreadyTo.output.find("hand_verdict=Usable") != std::string::npos);
    GC_EXPECT_TRUE(alreadyTo.output.find("reverse_total=1 reverse_emitted=1") != std::string::npos);
    char reverseFrom[40] {};
    std::snprintf(reverseFrom, sizeof(reverseFrom), "from=%#zx",
                  reinterpret_cast<size_t>(reverse.from));
    GC_EXPECT_TRUE(alreadyTo.output.find(reverseFrom) != std::string::npos);
    GC_EXPECT_TRUE(alreadyTo.output.find("reverse_overflow=0") != std::string::npos);

    reverse.from->SetStateCode(ObjectState::NORMAL);
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    reverse.region->metadata.liveInfo = nullptr;
    reverse.destination->metadata.liveInfo = nullptr;
    fx.FreePlanted(reverse.live);
    fx.FreePlanted(destinationLive);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// ZBarrier::is_good_or_null_fast_path does not send a load-good to-version
// through the from-side slow path (zBarrier.inline.hpp:294-343).  Reproduce the
// NW256 identity with two retired carriers: the source carrier retains an
// explicit from->to receipt, while the destination carrier has no receipt for
// the same numerical to-address.  Once the destination is no longer a current
// from range, TRACE incoming must keep the to-address and perform no lookup.
// The direct resolver arm is the positive control: a real current from-range
// member still consumes its retired ArmedHit receipt when the already-to TRACE
// guard is cut.
GC_TEST(ForwardingPublicationProduct, TraceIncomingAlreadyToOutsideFromSkipsLookup)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_TRACE);
    LateBackfillState reverse = PrepareLateBackfill(fx, collector);
    LiveInfo* destinationLive = PrepareForwardable(
        fx, reverse.destination, reinterpret_cast<MAddress>(reverse.to));
    {
        ForwardingTable::Publication publication = ForwardingTable::RetainOpenPublicationAfterCopy(
            reverse.region, reinterpret_cast<MAddress>(reverse.from));
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                         publication, reinterpret_cast<MAddress>(reverse.from),
                         reinterpret_cast<MAddress>(reverse.to)),
                     reinterpret_cast<MAddress>(reverse.to));
    }
    ForwardingTable::ClearEntries(reverse.region->GetRegionStart(), reverse.region->GetRegionSize());
    ForwardingTable::ClearEntries(
        reverse.destination->GetRegionStart(), reverse.destination->GetRegionSize());
    // Model the observed current to-region without destroying the historical
    // carrier: current membership is false by route state, while LookupTo can
    // still demonstrate the retired ArmedMiss that the old path consumed.
    reverse.destination->SetRouteState(RegionInfo::RouteState::NORMAL);

    GC_EXPECT_FALSE(collector.IsFromObject(reverse.to));
    GC_EXPECT_TRUE(Collector::JudgeHandOutTarget(reverse.to) == HandVerdict::Usable);

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    TraceBarrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    Mutator* const mutatorBefore = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&mutator);

    const uint64_t toHitsBefore = ForwardingTable::ArmedHitCount();
    const uint64_t toMissesBefore = ForwardingTable::ArmedMissCount();
    const uint64_t toUnavailableBefore = ForwardingTable::UnavailableCount();
    AbortCapture alreadyTo = CaptureAbort([&]() {
        barrier.WriteReference(holder, field, reverse.to);
        const bool correct = to_object(field.GetTargetObject()) == reverse.to &&
            collector.is_store_good(field) &&
            ForwardingTable::ArmedHitCount() == toHitsBefore &&
            ForwardingTable::ArmedMissCount() == toMissesBefore &&
            ForwardingTable::UnavailableCount() == toUnavailableBefore;
        if (!correct) {
            (void)dprintf(STDERR_FILENO,
                "ALREADY_TO_TRACE_BAD target=%p expected=%p hit_delta=%llu miss_delta=%llu unavailable_delta=%llu\n",
                to_object(field.GetTargetObject()), reverse.to,
                static_cast<unsigned long long>(ForwardingTable::ArmedHitCount() - toHitsBefore),
                static_cast<unsigned long long>(ForwardingTable::ArmedMissCount() - toMissesBefore),
                static_cast<unsigned long long>(ForwardingTable::UnavailableCount() - toUnavailableBefore));
            _exit(88);
        }
        (void)dprintf(STDERR_FILENO, "ALREADY_TO_TRACE_OK target=%p lookup_delta=0\n", reverse.to);
    });
    const bool alreadyToOk = WIFEXITED(alreadyTo.status) && WEXITSTATUS(alreadyTo.status) == 0 &&
        alreadyTo.output.find("ALREADY_TO_TRACE_OK") != std::string::npos;

    // Positive control for the zero-lookup claim: the same product resolver
    // must query and resolve a genuine from-range address.  Keep this to one
    // resolve so cutting the TRACE de-duplication guard cannot turn the
    // resulting to-version into a second, unrelated lookup.
    const uint64_t fromHitsBefore = ForwardingTable::ArmedHitCount();
    AbortCapture genuineFrom = CaptureAbort([&]() {
        BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, reverse.from);
        const bool correct = resolved == reverse.to &&
            ForwardingTable::ArmedHitCount() == fromHitsBefore + 1;
        if (!correct) {
            (void)dprintf(STDERR_FILENO,
                "GENUINE_FROM_TRACE_BAD target=%p expected=%p hit_delta=%llu\n",
                resolved, reverse.to,
                static_cast<unsigned long long>(ForwardingTable::ArmedHitCount() - fromHitsBefore));
            _exit(89);
        }
        (void)dprintf(STDERR_FILENO, "GENUINE_FROM_TRACE_OK from=%p to=%p hit_delta=1\n",
                      reverse.from, reverse.to);
    });
    const bool genuineFromOk = WIFEXITED(genuineFrom.status) && WEXITSTATUS(genuineFrom.status) == 0 &&
        genuineFrom.output.find("GENUINE_FROM_TRACE_OK") != std::string::npos;

    field.StoreColoured(zpointer::null);
    reverse.from->SetStateCode(ObjectState::NORMAL);
    if (reverse.region->IsGhostFromRegion()) {
        reverse.region->DispelGhostFromRegion();
    }
    if (reverse.destination->IsGhostFromRegion()) {
        reverse.destination->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-already-to-trace");
    reverse.region->metadata.liveInfo = nullptr;
    reverse.destination->metadata.liveInfo = nullptr;
    fx.FreePlanted(reverse.live);
    fx.FreePlanted(destinationLive);
    ThreadLocal::SetMutator(mutatorBefore);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    std::fprintf(stderr, "ALREADY_TO_TRACE_CHILD status=%d\n%s\n",
                 alreadyTo.status, alreadyTo.output.c_str());
    std::fprintf(stderr, "GENUINE_FROM_TRACE_CHILD status=%d\n%s\n",
                 genuineFrom.status, genuineFrom.output.c_str());
    GC_EXPECT_TRUE(alreadyToOk);
    GC_EXPECT_TRUE(genuineFromOk);
}

GC_TEST(ForwardingPublicationProduct, TraceOverwritePreviousCarriesRealHeapSource)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_TRACE);
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ZForwarding* table = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from));
    GC_EXPECT_TRUE(table != nullptr);
    const ZForwarding::FromPageView* fromPage = table->from_page_snapshot();
    GC_EXPECT_TRUE(fromPage != nullptr);
    const uint64_t generation = table->publication_generation();
    const uint64_t epoch = fromPage->epoch;
    const RegionLifeId lifeId = fromPage->lifeId;
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    BaseObject* holder = fx.obj0;
    auto& actualField = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    actualField.StoreColoured(ColouredPointer(state.from, OneLoadBadRemap()));
    TraceBarrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture aborted = CaptureAbort([&]() { barrier.WriteReference(holder, actualField, nullptr); });
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);

    char sourceToken[64] {};
    char generationToken[96] {};
    char epochToken[96] {};
    char lifeToken[96] {};
    (void)std::snprintf(sourceToken, sizeof(sourceToken), "source_slot=%p", &actualField);
    (void)std::snprintf(generationToken, sizeof(generationToken), "publication_generation=%llu",
                        static_cast<unsigned long long>(generation));
    (void)std::snprintf(epochToken, sizeof(epochToken), "from_page_epoch=%llu",
                        static_cast<unsigned long long>(epoch));
    (void)std::snprintf(lifeToken, sizeof(lifeToken), "lifeId=%llu",
                        static_cast<unsigned long long>(lifeId));
    const char* required[] = {
        "consumer=WCollector::TryUpdateRefFieldImpl",
        "stage=overwrite_previous",
        "writer_kind=write_reference",
        "incoming_source_kind=heap_ref_field",
        "field_type=ref_field",
        "field_offset=8",
        sourceToken,
        generationToken,
        epochToken,
        lifeToken,
    };
    for (const char* token : required) {
        GC_EXPECT_TRUE(aborted.output.find(token) != std::string::npos);
    }
    const std::string falseWorking = std::string("working_copy_slot=") +
        std::string(sourceToken + std::strlen("source_slot="));
    GC_EXPECT_TRUE(aborted.output.find("working_copy_slot=0x") != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find(falseWorking) == std::string::npos);

    actualField.StoreColoured(zpointer::null);
    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, IncomingRefStopsBeforeDestinationStore)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* incoming = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(incoming) + incoming->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_RECLAIM_SATB_NODE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(incoming));
    region->MarkForwardingDone();
    region->MarkForwardingDone();

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    gIncomingDestination = &field;
    gIncomingDestinationExpected = raw(field.GetFieldValue());
    Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture stopped = CaptureAbort([&]() {
        (void)signal(SIGABRT, IncomingAbortWitness);
        barrier.WriteReference(holder, field, incoming);
    });
    GC_EXPECT_TRUE(WIFEXITED(stopped.status));
    GC_EXPECT_EQ(WEXITSTATUS(stopped.status), 86);
    GC_EXPECT_TRUE(stopped.output.find("INCOMING_DESTINATION_UNCHANGED") != std::string::npos);
    GC_EXPECT_TRUE(stopped.output.find("stage=incoming_new") != std::string::npos);
    GC_EXPECT_TRUE(stopped.output.find("writer_kind=write_reference") != std::string::npos);
    GC_EXPECT_TRUE(stopped.output.find("incoming_source_kind=caller_value") != std::string::npos);
    GC_EXPECT_EQ(raw(field.GetFieldValue()), gIncomingDestinationExpected);
    gIncomingDestination = nullptr;

    field.StoreColoured(zpointer::null);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-incoming-stage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, LiveExactStartReceiptBeforeTraceOverwrite)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_TRACE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    RegionBitmap* mutableBitmap = live->GetMarkFace().bitmap;
    GC_EXPECT_TRUE(mutableBitmap != nullptr);
    GC_EXPECT_TRUE(region->LoadRouteStartTable()->count(0) == 1);
    // The exact-start set is the frozen producer input. Move the mutable face
    // to a later state so the test detects any producer that re-reads it.
    mutableBitmap->Reset();
    GC_EXPECT_FALSE(region->IsOwnerSurvivedObject(0));

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(from));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    const ForwardingTable::LookupResult identity = ForwardingTable::LookupTo(
        reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(identity.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(identity.to, reinterpret_cast<MAddress>(from));

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(ColouredPointer(from, OneLoadBadRemap()));
    TraceBarrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    Mutator* const mutatorBefore = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&mutator);
    barrier.WriteReference(holder, field, nullptr);
    ThreadLocal::SetMutator(mutatorBefore);
    GC_EXPECT_EQ(raw(field.GetFieldValue()), static_cast<uintptr_t>(0));

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-live-exact-trace");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, DeadOrUnselectedFromStillFailsClosed)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_TRACE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    GC_EXPECT_TRUE(region->LoadRouteStartTable()->count(0) == 0);

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(reinterpret_cast<MAddress>(dead)).answer ==
                   ForwardingTable::ToAnswer::Unavailable);

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(ColouredPointer(dead, OneLoadBadRemap()));
    TraceBarrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture aborted = CaptureAbort([&]() { barrier.WriteReference(holder, field, nullptr); });
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
    GC_EXPECT_TRUE(aborted.output.find("stage=overwrite_previous") != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("cause=publication_closed+never_installed") != std::string::npos);

    field.StoreColoured(zpointer::null);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-dead-exact-trace");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ArmedMissAfterPublicationCloseFailsClosed)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    const LookupWitnessIdentity expected = ReadLookupWitnessIdentity(
        ForwardingTable::GetEntries(reinterpret_cast<MAddress>(state.from)));
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    GC_EXPECT_TRUE(ForwardingTable::RetiredCovers(
        state.region->GetRegionStart(), state.region->GetRegionSize()));
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, state.from);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(result.unavailable_lookup_publication_closed());
    GC_EXPECT_TRUE(std::strcmp(result.unavailable_lookup_cause(),
                               "publication_closed+never_installed") == 0);
    GC_EXPECT_TRUE(std::strcmp(result.unavailable_lookup_retired_answer(), "armed_miss") == 0);
    GC_EXPECT_TRUE(result.unavailable_region_snapshot_valid());
    GC_EXPECT_EQ(result.unavailable_from(), reinterpret_cast<uintptr_t>(state.from));
    GC_EXPECT_NE(result.unavailable_from_region(), static_cast<uintptr_t>(0));
    std::fprintf(stderr, "LOOKUP_WITNESS_FINDTO table=%#zx generation=%llu epoch=%llu life=%llu\n",
                 static_cast<size_t>(result.unavailable_table_id()),
                 static_cast<unsigned long long>(result.unavailable_publication_generation()),
                 static_cast<unsigned long long>(result.unavailable_from_page_epoch()),
                 static_cast<unsigned long long>(result.unavailable_from_page_life_id()));
    GC_EXPECT_EQ(result.unavailable_table_id(), expected.tableId);
    GC_EXPECT_EQ(result.unavailable_publication_generation(), expected.generation);
    GC_EXPECT_EQ(result.unavailable_from_page_epoch(), expected.epoch);
    GC_EXPECT_EQ(result.unavailable_from_page_life_id(), expected.lifeId);

    RootSlot slot;
    StorePlain(slot, from_object(state.from));
    const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, state.from, &slot };
    AbortCapture aborted = CaptureAbort([&]() {
        (void)result.GetOrFailClosed(
            "ForwardingPublicationProduct.ArmedMissAfterPublicationCloseFailsClosed", provenance);
    });
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
    const char* required[] = {
        "[FINDTO][fail-closed]",
        "holder_kind=heap_ref",
        "slot=",
        "from=",
        "from_region=",
        "region_type=",
        "generation=",
        "in_current_relocation_set=",
        "table_id=",
        "lookup_state=unavailable",
        "cause=publication_closed+never_installed",
        "retired_lookup=armed_miss",
        "gc_phase=",
    };
    for (const char* token : required) {
        if (aborted.output.find(token) == std::string::npos) {
            std::fprintf(stderr, "ARMED_MISS_ABORT missing=%s\n---\n%s\n---\n",
                         token, aborted.output.c_str());
        }
        GC_EXPECT_TRUE(aborted.output.find(token) != std::string::npos);
    }

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardTaggedMissingScopeFailsClosed)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    collector.SetGCPhase(GCPhase::GC_PHASE_PREFORWARD);
    RootSlot root;
    StorePlain(root, from_object(fx.obj0));
    AbortCapture result = CaptureAbort([&]() { VisitTaggedOopSlot(root, false); });
    std::fprintf(stderr, "MISSING_BASE_MAP_RESULT status=%d\n%s", result.status, result.output.c_str());
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GC_EXPECT_TRUE(result.output.find("site=VisitTaggedOopSlot.preforward-base-map-missing") != std::string::npos);
    GC_EXPECT_TRUE(WIFSIGNALED(result.status));
    GC_EXPECT_EQ(WTERMSIG(result.status), SIGABRT);
}

GC_TEST(ForwardingPublicationProduct, PreForwardBaseMapScopeRestoresNestedScan)
{
    PreForwardBaseMapScope::Map outer;
    PreForwardBaseMapScope::Map inner;
    auto* previous = PreForwardBaseMapScope::Current();
    bool restoredOuter = false;
    bool isolatedThread = false;
    {
        PreForwardBaseMapScope outerScope(outer);
        GC_EXPECT_TRUE(PreForwardBaseMapScope::Current() == &outer);
        {
            PreForwardBaseMapScope innerScope(inner);
            GC_EXPECT_TRUE(PreForwardBaseMapScope::Current() == &inner);
        }
        restoredOuter = PreForwardBaseMapScope::Current() == &outer;
        std::thread worker([&]() {
            isolatedThread = PreForwardBaseMapScope::Current() == nullptr;
            PreForwardBaseMapScope workerScope(inner);
            isolatedThread = isolatedThread && PreForwardBaseMapScope::Current() == &inner;
        });
        worker.join();
        GC_EXPECT_TRUE(PreForwardBaseMapScope::Current() == &outer);
    }
    std::fprintf(stderr, "BASE_MAP_SCOPE_RESULT nested=%d isolated=%d restored=%d\n",
                 restoredOuter, isolatedThread, PreForwardBaseMapScope::Current() == previous);
    GC_EXPECT_TRUE(restoredOuter);
    GC_EXPECT_TRUE(isolatedThread);
    GC_EXPECT_TRUE(PreForwardBaseMapScope::Current() == previous);
}

// A managed frame is input data to the real mutator phase entry. Keep the
// descriptor in the loaded test image so the product metadata lifetime check
// can establish its identity; no stack scanner or resolver is replaced here.
#if defined(__x86_64__) && defined(__linux__)
namespace {
struct DerivedBaseMapImage {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t descriptorRest[6];
    uint8_t bits[256];
};
DerivedBaseMapImage derivedBaseMapImage;

void RunDerivedBaseProducer(bool interior, bool tagged, bool moving = false,
                            size_t interiorOffset = 8, bool expectFailClosed = false,
                            bool unresolvedGhost = false)
{
    auto& image = derivedBaseMapImage;
    std::memset(&image, 0, sizeof(image));
    image.descriptorOffset = reinterpret_cast<char*>(&image.stackMapOffset) -
        reinterpret_cast<char*>(&image.descriptorOffset);
    image.stackMapOffset = reinterpret_cast<char*>(image.bits) - reinterpret_cast<char*>(&image.stackMapOffset);
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(image.pc));
    size_t bit = 0;
    auto put = [&](uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            image.bits[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
        }
    };
    auto var = [&](uint32_t value) {
        if (value <= 11) { put(value, 4); }
        else { put(12, 4); put(value, 8); }
    };
    var(0); var(2); var(0); // stack size, uncompressed + tagged format, no saved registers
    var(1); var(0); var(2); var(0); var(1); // one PC, reg/slot/line/derived index widths
    if (CangjieRuntime::stackGrowConfig == StackGrowConfig::STACK_GROW_ON) { var(0); var(0); }
    var(0); var(1); var(0); // tagged reg/slot widths, padding
    put(0, 32); put(tagged ? 0 : 1, 2); put(1, 1); put(tagged ? 1 : 0, 1);
    var(0); var(0); // empty register table
    var(2); var(8); var(1); // two slot rows: base at fp-24, derived at fp-16
    put(232, 8); put(1, 1); put(240, 8); put(1, 1);
    var(0); var(0); // empty line table
    var(1); put(2, 2); // derived row selects second slot row

    GcHeapFixture& fx = ProductFixture();
    const U32 savedSize = fx.typeInfo->GetInstanceSize();
    if (interior && interiorOffset > 8) {
        fx.typeInfo->SetInstanceSize(128);
        std::memset(reinterpret_cast<char*>(fx.obj0) + sizeof(void*), 0, 128);
    }
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LateBackfillState state {};
    if (unresolvedGhost) {
        state = PrepareLateBackfill(fx, collector);
        state.from->SetStateCode(ObjectState::NORMAL);
        state.region->MarkForwardingDone();
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        RelocationReceiptTestAccess::ParkFrom(manager, state.region);
        GCWorkers workers(GCWorkers::Generation::OLD, 1);
        workers.SetActive();
        manager.ForwardFromRegions<Generation::Old>(workers);
        workers.SetInactive();
        auto owner = ForwardingTable::RetainPageOwner(state.region);
        const MAddress fromAddr = reinterpret_cast<MAddress>(state.from);
        const MAddress produced = owner ? owner->find(fromAddr) : 0;
        const bool completed = owner && owner->is_done() && owner->ref_count().load() == 0;
        std::fprintf(stderr, "DERIVED_PAGE_TASK produced=%zx expected=%zx done_released=%d\n",
                     produced, reinterpret_cast<MAddress>(state.to), completed);
        GC_EXPECT_TRUE(completed);
        GC_EXPECT_EQ(produced, reinterpret_cast<MAddress>(state.to));
        RelocationReceiptTestAccess::ReleaseListOwnership(state.region);
        state.region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        ForwardingCursor cursor = 0;
        GC_EXPECT_TRUE(owner->find(owner->index(fromAddr), &cursor).populated());
        owner->entries()[cursor].store(0, std::memory_order_release);
    } else if (moving) {
        state = PrepareValueRootForwarding(fx, collector);
    }
    collector.SetGCPhase(GCPhase::GC_PHASE_PREFORWARD);
    const bool usesState = moving || unresolvedGhost;
    const size_t usedOffset = interior ? interiorOffset : 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(usesState ? state.from : fx.obj0) + usedOffset;
    const uintptr_t expected = reinterpret_cast<uintptr_t>(moving ? state.to : fx.obj0) + usedOffset;
    uintptr_t frame[8] = {};
    frame[0] = base;
    frame[1] = base + 8;
    frame[2] = reinterpret_cast<uintptr_t>(image.pc) + 9;
    Mutator mutator;
    auto& context = mutator.GetUnwindContext();
    context.frameInfo.mFrame.SetIP(image.pc);
    context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frame[3]));
    context.anchorFA = nullptr;
    std::fprintf(stderr, "DERIVED_BASE_INPUT interior=%d tagged=%d moving=%d offset=%zu base=%zx derived=%zx\n",
                 interior, tagged, moving, usedOffset, frame[0], frame[1]);
    if (expectFailClosed) {
        AbortCapture aborted = CaptureAbort([&]() {
            mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_PREFORWARD, false);
        });
        std::fprintf(stderr, "DERIVED_BASE_FAILCLOSED status=%d\n%s", aborted.status, aborted.output.c_str());
        fx.typeInfo->SetInstanceSize(savedSize);
        if (usesState) { CleanupLateBackfill(fx, state); }
        collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
        std::fprintf(stderr, "DERIVED_BASE_TARGET target_assertion executed=1 matched=%d\n",
                     aborted.output.find("site=Mutator::MakePreForwardDerivedVisitor.base-not-remapped") !=
                         std::string::npos);
        GC_EXPECT_TRUE(aborted.output.find("site=Mutator::MakePreForwardDerivedVisitor.base-not-remapped") !=
                       std::string::npos);
        GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
        GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
        return;
    }
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_PREFORWARD, false);
    std::fprintf(stderr, "DERIVED_BASE_RESULT base=%zx derived=%zx expected=%zx\n", frame[0], frame[1], expected + 8);
    const bool baseCorrect = frame[0] == expected;
    const bool derivedCorrect = frame[1] == expected + 8;
    fx.typeInfo->SetInstanceSize(savedSize);
    if (usesState) { CleanupLateBackfill(fx, state); }
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GC_EXPECT_TRUE(derivedCorrect);
    GC_EXPECT_TRUE(baseCorrect);
}
}

GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedInteriorBaseProducer)
{
    RunDerivedBaseProducer(true, false);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedBaseProducer)
{
    RunDerivedBaseProducer(false, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedOrdinaryBaseProducer)
{
    RunDerivedBaseProducer(false, false);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedInteriorMovingBaseProducer)
{
    RunDerivedBaseProducer(true, false, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedMovingBaseProducer)
{
    RunDerivedBaseProducer(false, true, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedOrdinaryMovingBaseProducer)
{
    RunDerivedBaseProducer(false, false, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedInteriorUnrecoveredHostFailsClosed)
{
    RunDerivedBaseProducer(true, false, false, 80, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedUnresolvedGhostFailsClosed)
{
    RunDerivedBaseProducer(false, true, false, 8, true, true);
}
#endif

GC_TEST(ForwardingPublicationProduct, PreForwardDerivedRebasesFromRemappedBaseWithoutLookup)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    GC_EXPECT_TRUE(ForwardingTable::RetiredCovers(
        state.region->GetRegionStart(), state.region->GetRegionSize()));

    constexpr size_t derivedOffset = sizeof(uintptr_t);
    RootSlot oldBase;
    StorePlain(oldBase, from_object(state.from));
    DerivedSlot derived;
    RebaseDerived(derived, oldBase, derivedOffset);

    const uint64_t hitsBefore = ForwardingTable::ArmedHitCount();
    const uint64_t missesBefore = ForwardingTable::ArmedMissCount();
    const uint64_t unavailableBefore = ForwardingTable::UnavailableCount();
    const uint64_t unarmedBefore = ForwardingTable::UnarmedCount();
    size_t resolverCalls = 0;
    DerivedPtrVisitor visitor = Mutator::MakePreForwardDerivedVisitor(
        [&](BaseObject* old) -> BaseObject* {
            ++resolverCalls;
            GC_EXPECT_TRUE(old == state.from);
            return state.to;
        });
    visitor(oldBase.LoadPlain(), derived);

    GC_EXPECT_EQ(resolverCalls, static_cast<size_t>(1));
    GC_EXPECT_EQ(raw(derived.LoadDerived()),
                 reinterpret_cast<MAddress>(state.to) + derivedOffset);
    GC_EXPECT_EQ(ForwardingTable::ArmedHitCount(), hitsBefore);
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), missesBefore);
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), unavailableBefore);
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), unarmedBefore);

    // Positive control for the zero-lookup assertion above: the same closed carrier and old base
    // must move the unavailable counter when the forwarding lookup is explicitly invoked.
    FindToVersionResult lookup = RelocationReceiptTestAccess::ProductFindToVersion(collector, state.from);
    GC_EXPECT_TRUE(lookup.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), unavailableBefore + 1);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ArmedHitAfterPublicationCloseResolves)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    {
        ForwardingTable::Publication publication =
            ForwardingTable::RetainOpenPublicationAfterCopy(
                state.region, reinterpret_cast<MAddress>(state.from));
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(
                         publication, reinterpret_cast<MAddress>(state.from),
                         reinterpret_cast<MAddress>(state.to)),
                     reinterpret_cast<MAddress>(state.to));
    }
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, state.from);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::Found);
    GC_EXPECT_TRUE(result.found() == state.to);
    RootSlot slot;
    StorePlain(slot, from_object(state.from));
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };
    GC_EXPECT_TRUE(result.GetOrFailClosed(
                       "ForwardingPublicationProduct.ArmedHitAfterPublicationCloseResolves",
                       provenance) == state.to);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, UnlinkMissReportsTableDestroyed)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    DestroyAfterGhostCleared(state.region, "gc-unit-explicit-coverage");
    GC_EXPECT_FALSE(ForwardingTable::RetiredCovers(
        state.region->GetRegionStart(), state.region->GetRegionSize()));

    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, state.from);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_TRUE(std::strcmp(result.unavailable_lookup_cause(),
                               "publication_closed+table_destroyed") == 0);
    GC_EXPECT_TRUE(std::strcmp(result.unavailable_lookup_retired_answer(), "unarmed") == 0);
    GC_EXPECT_EQ(result.unavailable_table_id(), static_cast<uintptr_t>(0));

    RootSlot slot;
    StorePlain(slot, from_object(state.from));
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };
    AbortCapture aborted = CaptureAbort([&]() {
        (void)result.GetOrFailClosed(
            "ForwardingPublicationProduct.UnlinkMissReportsTableDestroyed", provenance);
    });
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
    GC_EXPECT_TRUE(aborted.output.find("holder_kind=static") != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("cause=publication_closed+table_destroyed") != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("retired_lookup=unarmed") != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("table_id=0") != std::string::npos);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// A non-LookupUnavailable route may carry lookup-shaped fields from a caller,
// but with the snapshot validity bit cleared they must never be rendered as
// legal-looking zero values.
GC_TEST(FindToRouteDiagnostics, InvalidLookupSnapshotPrintsNa)
{
    FindToVersionResult::UnavailableWitness witness;
    witness.lookupAnswer = "unarmed";
    witness.lookupSnapshotValid = true;
    witness.lookupCause = "publication_closed";
    witness.lookupActiveCandidate = true;
    witness.lookupActiveAnswer = "armed_hit";
    witness.lookupRetiredAnswer = "armed_miss";
    witness.lookupPublicationClosed = true;
    // Deliberately clear the one validity bit for this whole LookupTo record.
    // Every lookup-shaped value above must consequently render as n/a.
    witness.lookupSnapshotValid = false;
    const FindToVersionResult result = FindToVersionResult::Unavailable(
        FindToVersionResult::UnavailableRoute::NoGhostForwarded, witness);
    RootSlot slot;
    const ForwardingProvenance provenance{ ForwardingHolderKind::Static, nullptr, &slot };

    ExpectRootAbortAt("lookup=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
    ExpectRootAbortAt("cause=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
    ExpectRootAbortAt("active_candidate=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
    ExpectRootAbortAt("active_lookup=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
    ExpectRootAbortAt("retired_lookup=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
    ExpectRootAbortAt("publication_closed=n/a", [&]() {
        (void)result.GetOrFailClosed("FindToRouteDiagnostics.InvalidLookupSnapshotPrintsNa", provenance);
    });
}

GC_TEST(ForwardingPublicationProduct, ExemptRejectsForwardedWithoutAnyReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    from->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(from)), 0);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)), 0);

    RegionManager manager;
    ExpectRootAbortAt("forwarded object lacks receipt before kept-page retirement", [&]() {
        manager.ExemptFromRegion(region);
    });

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-forwarded-without-receipt");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// SD forwarding consumer gate: a compacted destination can be classified as
// kAlreadyToStart by reverse geometry, but that classification is not a
// load-good receipt. Make the destination header FORWARDED and drive the
// product ResolveStoreValue entry; the only legal result is fail-closed.
GC_TEST(ForwardingPublicationProduct, ResolveStoreValueAlreadyToStartRejectsNonUsable)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    PartialCompactState state = PreparePartialCompact(fx, collector, true);

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, state.region);
    manager.CompactRegion(state.region, state.destination);
    state.region->MarkForwardingDone();

    BaseObject* compactedStart = from_region_addr(state.region->GetRegionStart());
    compactedStart->SetStateCode(ObjectState::FORWARDED);
    ExpectRootAbortAt("[fail-closed]", [&]() {
        (void)RelocationReceiptTestAccess::ResolveStoreValue(collector, compactedStart);
    });

    compactedStart->SetStateCode(ObjectState::NORMAL);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
#endif
}

GC_TEST(ForwardingPublicationProduct, ResolveStoreValueAlreadyToStartWithUsableTarget)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    PartialCompactState state = PreparePartialCompact(fx, collector, true);

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, state.region);
    manager.CompactRegion(state.region, state.destination);
    state.region->MarkForwardingDone();

    BaseObject* compactedStart = from_region_addr(state.region->GetRegionStart());
    compactedStart->SetStateCode(ObjectState::NORMAL);
    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, compactedStart);
    GC_EXPECT_TRUE(resolved != nullptr);
    GC_EXPECT_TRUE(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
#endif
}

GC_TEST(ForwardingPublicationProduct, MarkForwardingDoneClosedReceipts)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    RegionManager manager;
    manager.ExemptFromRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(lookup.to == reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-closed-receipts");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, MarkForwardingDoneRejectsReceiptCountMismatch)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* first = fx.PlaceObject(region->GetRegionStart());
    BaseObject* second = fx.PlaceObject(region->GetRegionStart() + first->GetSize());
    BaseObject* third = fx.PlaceObject(region->GetRegionStart() + first->GetSize() + second->GetSize());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(third) + third->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(first));
    RegionBitmap* bitmap = region->GetMarkBitmap(region->GetMarkView<Generation::Old>());
    GC_EXPECT_TRUE(bitmap != nullptr);
    const size_t secondOff = region->GetAddressOffset(reinterpret_cast<MAddress>(second));
    const size_t thirdOff = region->GetAddressOffset(reinterpret_cast<MAddress>(third));
    (void)bitmap->MarkBits(secondOff, second->GetSize(), region->GetRegionSize());
    (void)bitmap->MarkBits(thirdOff, third->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(second->GetSize());
    region->AddLiveByteCount(third->GetSize());
    region->RecordRouteStart(secondOff);
    region->RecordRouteStart(thirdOff);

    RegionManager manager;
    manager.ExemptFromRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(first)),
                 reinterpret_cast<MAddress>(first));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(second)),
                 reinterpret_cast<MAddress>(second));
    GC_EXPECT_EQ(ForwardingTable::FindTo(reinterpret_cast<MAddress>(third)),
                 reinterpret_cast<MAddress>(third));

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-receipt-count");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, LookupCausePublishedWithoutReceipt)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    const LookupWitnessIdentity expected = ReadLookupWitnessIdentity(
        ForwardingTable::GetEntries(reinterpret_cast<MAddress>(from)));
    region->MarkForwardingDone();
    region->MarkForwardingDone();
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(lookup.to == 0);
    GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit);

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture aborted = CaptureAbort([&]() { barrier.WriteReference(holder, field, from); });
    std::fprintf(stderr, "LOOKUP_WITNESS_WAIT status=%d\n%s", aborted.status, aborted.output.c_str());
    if (!WIFSIGNALED(aborted.status)) {
        std::fprintf(stderr, "WAIT_PROVENANCE status=%d output=\n%s\n", aborted.status,
                     aborted.output.c_str());
    }
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
    ExpectDiagnosticLookupIdentity(aborted.output, expected);
    const char* required[] = {
        "WCollector::WaitRoutedTipReady.published-without-receipt",
        "holder_kind=heap_ref",
        "holder=",
        "slot=",
        "waiter=",
        "from=",
        "from_region=",
        "table_id=",
        "expected_publisher=",
        "lookup_state=",
        "lookup_cause=",
        "retired_lookup=",
        "gc_phase=",
    };
    for (const char* token : required) {
        if (aborted.output.find(token) == std::string::npos) {
            std::fprintf(stderr, "WAIT_PROVENANCE missing=%s\n---\n%s\n---\n",
                         token, aborted.output.c_str());
        }
        GC_EXPECT_TRUE(aborted.output.find(token) != std::string::npos);
    }
    char holderToken[64] {};
    char slotToken[64] {};
    (void)std::snprintf(holderToken, sizeof(holderToken), "holder=%p", holder);
    (void)std::snprintf(slotToken, sizeof(slotToken), "slot=%p", &field);
    GC_EXPECT_TRUE(aborted.output.find(holderToken) != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find(slotToken) != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("holder_kind=unknown") == std::string::npos);

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-lookup-cause");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
#endif
}

// These observations are emitted by the SO after the public store barrier has
// resolved its value. The fixture never calls WaitRouted or formats its output.
static std::string ObservationLine(const std::string& output, const char* marker)
{
    const size_t begin = output.find(marker);
    return begin == std::string::npos ? std::string{} : output.substr(begin, output.find('\n', begin) - begin);
}

static std::string ObservationField(const std::string& line, const char* field)
{
    const std::string key = std::string(" ") + field + "=";
    const size_t keyAt = line.find(key);
    if (keyAt == std::string::npos) {
        return "<missing>";
    }
    const size_t begin = keyAt + key.size();
    return line.substr(begin, line.find(' ', begin) - begin);
}

static unsigned CheckObservationField(const std::string& actual, const std::string& expected,
                                      const char* field, const char* site)
{
    const std::string got = ObservationField(actual, field);
    const std::string want = ObservationField(expected, field);
    const bool equal = want != "<missing>" && got == want;
    // Keep checking after a failed field so a cut cannot hide behind an earlier
    // presence assertion. Each product result reaches its own visible predicate.
    std::fprintf(stderr, "OBS_ASSERT site=%s field=%s actual=%s expected=%s result=%s\n",
                 site, field, got.c_str(), want.c_str(), equal ? "PASS" : "FAIL");
    return equal ? 0 : 1;
}

enum class WaitObservationCase {
    PublishedMiss, InitialIdentity, Moved, UnpublishedIdentity, IneligibleIdentity,
    ClosedReturn, RetainRefusedIdentity, RequestIdentity, TerminalIdentity
};

#if defined(MRT_FINDTO_RETAIN_TEST)
struct WaitObservationPublication {
    RegionInfo* region;
    BaseObject* from;
    BaseObject* destination;
    WaitObservationCase scenario;
    unsigned lookups{ 0 };
    MAddress published{ 0 };
    static void Publish(void* context)
    {
        auto& state = *static_cast<WaitObservationPublication*>(context);
        // ResolveStoreValue's FindToVersion, then AdmitForRoute/GetRoute in
        // each of the two ComputeRoute calls, precede WaitRouted's first lookup.
        // The return.kind predicate below independently checks this schedule.
        ++state.lookups;
        const auto scenario = state.scenario;
        if (scenario == WaitObservationCase::RetainRefusedIdentity && state.lookups == 6) {
            state.region->ReleaseForwarding();
        }
        if (scenario == WaitObservationCase::TerminalIdentity && state.lookups == 7) {
            state.region->MarkForwardingDone();
        }
        if (scenario == WaitObservationCase::ClosedReturn) {
            if (state.lookups == 7) {
                ProductForcePublicationClosedForTestFn()(reinterpret_cast<MAddress>(state.from));
            }
            return;
        }
        unsigned publishAt = 6;
        if (scenario == WaitObservationCase::IneligibleIdentity ||
            scenario == WaitObservationCase::RetainRefusedIdentity ||
            scenario == WaitObservationCase::RequestIdentity) {
            publishAt = 7;
        } else if (scenario == WaitObservationCase::TerminalIdentity) {
            publishAt = 8;
        }
        if (state.lookups == publishAt) {
            const MAddress from = reinterpret_cast<MAddress>(state.from);
            auto publication = ForwardingTable::RetainOpenPublicationAfterCopy(state.region, from);
            const MAddress to = ForwardingTable::InsertMapping(
                publication, from, reinterpret_cast<MAddress>(state.destination));
            state.published = to;
            if (scenario == WaitObservationCase::UnpublishedIdentity) {
                state.from->SetClassInfo(nullptr);
            }
            std::fprintf(stderr, "OBS_PUBLICATION lookups=%u from=%#zx to=%#zx\n", state.lookups, from, to);
        }
    }
};
#endif

static void CheckWaitObservation(WaitObservationCase scenario)
{
    const bool moved = scenario == WaitObservationCase::Moved;
    const bool miss = scenario == WaitObservationCase::PublishedMiss;
    const bool closed = scenario == WaitObservationCase::ClosedReturn;
    const bool identity = !miss && !moved && !closed;
    const bool queued = scenario == WaitObservationCase::RequestIdentity ||
        scenario == WaitObservationCase::TerminalIdentity;
    const bool eligible = queued || scenario == WaitObservationCase::RetainRefusedIdentity;
    const char* const kinds[] = { "published-without-receipt", "initial-lookup", "quiet",
        "unpublished-use-to", "ineligible-lookup", "ineligible-closed", "retain-refused-lookup",
        "request-receipt", "terminal-lookup" };
    const char* kind = kinds[static_cast<unsigned>(scenario)];
    GcHeapFixture& fx = ProductFixture();
    unsigned failures = 0;
    for (unsigned sample = 0; sample != 2; ++sample) {
        AbortCapture captured = CaptureAbort([&]() {
            RegionInfo* region = RegionInfo::GetRegionInfo(4 + sample);
            RelocationReceiptTestAccess::ReleaseListOwnership(region);
            region = RegionInfo::InitRegion(4 + sample, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
            if (sample != 0) {
                region->BumpRegionLifeId();
            }
            region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
            for (unsigned bump = 0; bump <= sample; ++bump) {
                region->BumpSnapshotEpoch();
            }
            BaseObject* from = fx.PlaceObject(region->GetRegionStart());
            region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
            WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
            collector.SetGCPhase(miss ? GCPhase::GC_PHASE_FORWARD
                : eligible ? GCPhase::GC_PHASE_POST_TRACE : GCPhase::GC_PHASE_IDLE);
            collector.flip_old_relocate_start();
            RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
            (void)PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
            region->SetRouteState(miss ? RegionInfo::RouteState::COMPACTED : RegionInfo::RouteState::ROUTED);
            if (miss || (scenario == WaitObservationCase::InitialIdentity && sample != 0)) {
                region->MarkForwardingDone();
            }
            if (queued) {
                collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
                BaseObject* got = RelocationReceiptTestAccess::WaitRoutedTipReady(
                    collector, from, nullptr, region);
                std::fprintf(stderr, "OBS_QUEUED_ENTRY got=%p from=%p\n",
                             static_cast<void*>(got), static_cast<void*>(from));
                _exit(0);
            }
            g_gcCount.store(701 + sample, std::memory_order_relaxed);
            const ForwardingTable::LookupResult before = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
            const uint64_t routeSnapshot = region->metadata.routeStateSnapshot.load(std::memory_order_acquire);
            std::fprintf(stderr,
                "OBS_EXPECT tid=%d obj=%p region=%p gcCycle=%zu returned=%p resolved=%p "
                "route=%u route.snapshot=%#llx fwdDone=%u lookup.answer=%u lookup.to=%#zx "
                "tableId=%#zx epoch=%llu lifeId=%llu publicationGeneration=%llu lookup.snapshot.valid=%u return.kind=%s "
                "active.answer=%u retired.answer=%u route.decision.valid=%u lookup.record=WaitRouted.return\n",
                static_cast<int>(getpid()), static_cast<void*>(from), static_cast<void*>(region),
                g_gcCount.load(std::memory_order_relaxed), identity ? static_cast<void*>(from) : nullptr,
                identity ? static_cast<void*>(from) : nullptr,
                static_cast<unsigned>(RegionInfo::RouteStateFromSnapshot(routeSnapshot)),
                static_cast<unsigned long long>(routeSnapshot),
                static_cast<unsigned>(region->IsForwardingDone() || scenario == WaitObservationCase::TerminalIdentity),
                static_cast<unsigned>(identity ? ForwardingTable::ToAnswer::ArmedHit
                    : closed ? ForwardingTable::ToAnswer::Unavailable : before.answer),
                identity ? reinterpret_cast<size_t>(from) : static_cast<size_t>(before.to),
                static_cast<size_t>(before.tableId), static_cast<unsigned long long>(before.fromPageEpoch),
                static_cast<unsigned long long>(before.fromPageLifeId),
                static_cast<unsigned long long>(before.publicationGeneration),
                static_cast<unsigned>(before.forwardingSnapshotValid), kind,
                static_cast<unsigned>(identity ? ForwardingTable::ToAnswer::ArmedHit : before.activeAnswer),
                static_cast<unsigned>(before.retiredAnswer),
                static_cast<unsigned>(scenario != WaitObservationCase::InitialIdentity && !moved));
#if defined(MRT_FINDTO_RETAIN_TEST)
            WaitObservationPublication publication{ region, from, moved ? fx.obj1 : from, scenario };
            if (!miss) {
                ProductSetLookupRetainHookFn()(&WaitObservationPublication::Publish, &publication);
            }
#endif
            BaseObject* holder = fx.obj0;
            auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
            Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
            std::fprintf(stderr, "OBS_ENTRY Barrier::WriteReference\n");
            barrier.WriteReference(holder, field, from);
#if defined(MRT_FINDTO_RETAIN_TEST)
            if (moved) {
                BaseObject* stored = to_object(field.GetTargetObject());
                std::fprintf(stderr, "OBS_MOVED stored=%p expected=%p lookups=%u\n",
                             static_cast<void*>(stored), static_cast<void*>(fx.obj1), publication.lookups);
                _exit(stored == fx.obj1 && publication.published == reinterpret_cast<MAddress>(fx.obj1) ? 0 : 91);
            }
#endif
            std::fprintf(stderr, "OBS_UNEXPECTED_RETURN\n");
        });
        std::fprintf(stderr, "OBS_CHILD kind=%s sample=%u status=%d\n%s\n",
                     kind, sample, captured.status, captured.output.c_str());
        const std::string expected = ObservationLine(captured.output, "OBS_EXPECT");
        const std::string waited = ObservationLine(captured.output, "[WaitRouted.return]");
        if (moved) {
            const bool completed = WIFEXITED(captured.status) && WEXITSTATUS(captured.status) == 0 &&
                captured.output.find("OBS_MOVED") != std::string::npos && waited.empty();
            std::fprintf(stderr, "OBS_ASSERT site=behavior field=moved-receipt-without-log result=%s\n",
                         completed ? "PASS" : "FAIL");
            failures += completed ? 0 : 1;
            continue;
        }
        if (queued) {
            const bool completed = WIFEXITED(captured.status) && WEXITSTATUS(captured.status) == 0 &&
                captured.output.find("OBS_QUEUED_ENTRY") != std::string::npos;
            std::fprintf(stderr, "OBS_ASSERT site=behavior field=queued-entry result=%s\n",
                         completed ? "PASS" : "FAIL");
            failures += completed ? 0 : 1;
            continue;
        }
        const bool terminated = WIFSIGNALED(captured.status) && WTERMSIG(captured.status) == SIGABRT;
        std::fprintf(stderr, "OBS_ASSERT site=behavior field=termination result=%s\n", terminated ? "PASS" : "FAIL");
        failures += terminated ? 0 : 1;
        const char* fields[] = { "tid", "obj", "region", "gcCycle", "returned", "route", "route.snapshot",
            "fwdDone", "lookup.answer", "lookup.to", "tableId", "epoch", "lifeId", "publicationGeneration",
            "lookup.snapshot.valid", "return.kind", "active.answer", "retired.answer", "route.decision.valid" };
        for (const char* field : fields) {
            failures += CheckObservationField(waited, expected, field, "wait");
        }
        if (identity || closed) {
            const std::string outer = ObservationLine(captured.output,
                "ZRelocate::forward_object requires a forwarding entry");
            const char* outerFields[] = { "tid", "obj", "region", "gcCycle", "resolved", "route.snapshot",
                "fwdDone", "lookup.record" };
            for (const char* field : outerFields) {
                failures += CheckObservationField(outer, expected, field, "check");
            }
            for (const char* field : { "tid", "obj", "gcCycle" }) {
                failures += CheckObservationField(outer, waited, field, "pair");
            }
            const bool fatalSite = !outer.empty();
            std::fprintf(stderr, "OBS_ASSERT site=behavior field=fatal-site result=%s\n", fatalSite ? "PASS" : "FAIL");
            failures += fatalSite ? 0 : 1;
        } else {
            const bool fatalSite = captured.output.find("WCollector::WaitRoutedTipReady.published-without-receipt")
                != std::string::npos;
            std::fprintf(stderr, "OBS_ASSERT site=behavior field=fatal-site result=%s\n", fatalSite ? "PASS" : "FAIL");
            failures += fatalSite ? 0 : 1;
        }
    }
    GC_EXPECT_EQ(failures, 0u);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationPublishedMiss)
{
    CheckWaitObservation(WaitObservationCase::PublishedMiss);
}

#if defined(MRT_FINDTO_RETAIN_TEST)
GC_TEST(ForwardingPublicationProduct, WaitObservationIdentity)
{
    CheckWaitObservation(WaitObservationCase::InitialIdentity);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationMovedReceiptIsQuiet)
{
    CheckWaitObservation(WaitObservationCase::Moved);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationUnpublishedIdentity)
{
    CheckWaitObservation(WaitObservationCase::UnpublishedIdentity);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationIneligibleIdentity)
{
    CheckWaitObservation(WaitObservationCase::IneligibleIdentity);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationClosedReturn)
{
    CheckWaitObservation(WaitObservationCase::ClosedReturn);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationRetainRefusedIdentity)
{
    CheckWaitObservation(WaitObservationCase::RetainRefusedIdentity);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationRequestIdentity)
{
    CheckWaitObservation(WaitObservationCase::RequestIdentity);
}

GC_TEST(ForwardingPublicationProduct, WaitObservationTerminalIdentity)
{
    CheckWaitObservation(WaitObservationCase::TerminalIdentity);
}
#endif

GC_TEST(ForwardingPublicationProduct, CompactedWithoutFwdDoneWaitsInProductSO)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_FALSE(region->IsForwardingDone());
    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& queue = productSpace.GetRegionManager().GetRelocationRequestQueue();
    queue.BeginWorkers(1);

    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        (void)RelocationReceiptTestAccess::WaitRoutedTipReady(collector, from, nullptr, region);
        _exit(0);
    }
    int status = 0;
    bool aborted = false;
    for (int i = 0; i < 50; ++i) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!aborted) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    GC_EXPECT_FALSE(aborted);
    (void)queue.SynchronizePoll();

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    ForwardingTable::ReclaimRetired("gc-unit-compacted-wait-so");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
#endif
}
#endif

GC_TEST(ForwardingPublicationProduct, ForwardUpdateRawRefWritesBackMappedTo)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(region->GetRegionStart() + 256);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    (void)ForwardingTable::InstallMapping(publication, reinterpret_cast<MAddress>(from),
                                          reinterpret_cast<MAddress>(to));
    region->MarkForwardingDone();
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    ObjectRef root;
    StorePlain(root, from_object(from));
    BaseObject* resolved = RelocationReceiptTestAccess::ForwardUpdateRawRef(collector, root);
    GC_EXPECT_TRUE(resolved == to);
    HeapSlot<> bits(to_zpointer(raw(root.LoadPlain())));
    GC_EXPECT_TRUE(to_object(bits.GetTargetObject()) == to);

    publication = ForwardingTable::Publication();
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ForwardUpdateRawRefFailClosedWhenUnresolved)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    ExpectRootAbortAt("ForwardUpdateRawRef.unresolved", [&]() {
        ObjectRef root;
        StorePlain(root, from_object(from));
        (void)RelocationReceiptTestAccess::ForwardUpdateRawRef(collector, root);
    });
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
#endif
}

GC_TEST(ForwardingPublicationProduct, IdentityForwardStillWritesBackRootWord)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    (void)ForwardingTable::InstallMapping(publication, reinterpret_cast<MAddress>(from),
                                          reinterpret_cast<MAddress>(from));
    region->MarkForwardingDone();
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    const uintptr_t colored = reinterpret_cast<uintptr_t>(from) |
        (static_cast<uintptr_t>(::g_cjLoadBadMask) ^ REMAP_COLOUR_MASK);
    ObjectRef root;
    StorePlain(root, to_zaddress(colored));
    GC_EXPECT_TRUE(raw(root.LoadPlain()) != reinterpret_cast<MAddress>(from));
    BaseObject* resolved = RelocationReceiptTestAccess::ForwardUpdateRawRef(collector, root);
    GC_EXPECT_TRUE(resolved == from);
    GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<MAddress>(from));

    publication = ForwardingTable::Publication();
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, FixRootInteriorFailClosedWhenHostUnresolved)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    ExpectRootAbortAt("FixMinorEvacuatedSlot.interior-unresolved", [&]() {
        ObjectRef root;
        StorePlain(root, to_zaddress(reinterpret_cast<MAddress>(from) + 8));
        (void)RelocationReceiptTestAccess::FixMinorRoot(collector, root);
    });
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
#endif
}

GC_TEST(ForwardingPublicationProduct, ResolveStoreValueFollowsForwardedDestination)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(5));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* firstRegion = RegionInfo::InitRegion(5, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* secondRegion = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* finalRegion = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(firstRegion != nullptr && secondRegion != nullptr && finalRegion != nullptr);
    firstRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    secondRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    finalRegion->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* first = fx.PlaceObject(firstRegion->GetRegionStart());
    BaseObject* second = fx.PlaceObject(secondRegion->GetRegionStart());
    BaseObject* final = fx.PlaceObject(finalRegion->GetRegionStart());
    firstRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(first) + first->GetSize());
    secondRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    finalRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(final) + final->GetSize());

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* firstLive = PrepareForwardable(fx, firstRegion, reinterpret_cast<MAddress>(first));
    LiveInfo* secondLive = PrepareForwardable(fx, secondRegion, reinterpret_cast<MAddress>(second));
    ForwardingTable::Publication firstPublication =
        ForwardingTable::EnsurePublicationBeforeCopy(firstRegion, reinterpret_cast<MAddress>(first));
    ForwardingTable::Publication secondPublication =
        ForwardingTable::EnsurePublicationBeforeCopy(secondRegion, reinterpret_cast<MAddress>(second));
    GC_EXPECT_TRUE(static_cast<bool>(firstPublication));
    GC_EXPECT_TRUE(static_cast<bool>(secondPublication));
    (void)ForwardingTable::InstallMapping(firstPublication, reinterpret_cast<MAddress>(first),
                                          reinterpret_cast<MAddress>(second));
    (void)ForwardingTable::InstallMapping(secondPublication, reinterpret_cast<MAddress>(second),
                                          reinterpret_cast<MAddress>(final));
    first->SetStateCode(ObjectState::FORWARDED);
    second->SetStateCode(ObjectState::FORWARDED);

    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, first);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(resolved), reinterpret_cast<MAddress>(final));
    GC_EXPECT_TRUE(Collector::JudgeHandOutTarget(resolved) == HandVerdict::Usable);

    firstPublication = ForwardingTable::Publication();
    secondPublication = ForwardingTable::Publication();
    ForwardingTable::ClearEntries(firstRegion->GetRegionStart(), firstRegion->GetRegionSize());
    ForwardingTable::ClearEntries(secondRegion->GetRegionStart(), secondRegion->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (firstRegion->IsGhostFromRegion()) {
        firstRegion->DispelGhostFromRegion();
    }
    if (secondRegion->IsGhostFromRegion()) {
        secondRegion->DispelGhostFromRegion();
    }
    firstRegion->metadata.liveInfo = nullptr;
    secondRegion->metadata.liveInfo = nullptr;
    fx.FreePlanted(firstLive);
    fx.FreePlanted(secondLive);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, PartialCompactFirstDestinationKeepsReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    PartialCompactState state = PreparePartialCompact(fx, collector, false);
    const MAddress from = reinterpret_cast<MAddress>(state.liveObject);
    const MAddress expected = state.destination->GetRegionStart();

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, state.region);
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(state.region, from);
    GC_EXPECT_TRUE(request.accepted);

    manager.CompactRegion(state.region, state.destination);

    (void)queue.Wait(request.request);
    const MAddress receipt = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(receipt, expected);
    GC_EXPECT_TRUE(receipt != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), expected);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(expected)->IsValidObject());
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
}

GC_TEST(ForwardingPublicationProduct, PartialCompactSelfFallbackKeepsReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    PartialCompactState state = PreparePartialCompact(fx, collector, true);
    const MAddress from = reinterpret_cast<MAddress>(state.liveObject);
    const MAddress expected = state.region->GetRegionStart();

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, state.region);
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(state.region, from);
    GC_EXPECT_TRUE(request.accepted);

    manager.CompactRegion(state.region, state.destination);
    state.region->MarkForwardingDone();

    (void)queue.Wait(request.request);
    const MAddress receipt = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(receipt, expected);
    GC_EXPECT_TRUE(receipt != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), expected);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(expected)->IsValidObject());
    RefField<> qualified = RelocationReceiptTestAccess::QualifyStoreValue(
        collector, reinterpret_cast<BaseObject*>(expected));
    GC_EXPECT_EQ(raw(qualified.GetTargetObject()), expected);
    RefField<> productField(qualified);
    (void)RelocationReceiptTestAccess::FixMinorField(collector, productField);
    GC_EXPECT_EQ(raw(productField.GetTargetObject()), expected);
    RefField<> derivedField(expected + 8u);
    (void)RelocationReceiptTestAccess::FixMinorField(
        collector, derivedField, reinterpret_cast<BaseObject*>(expected));
    GC_EXPECT_EQ(raw(derivedField.GetTargetObject()), expected + 8u);
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
}

GC_TEST(ForwardingPublicationProduct, PageWaitThenLookupReadsOriginalCompactReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* routeDestination =
        RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && routeDestination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + objectSize);
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    const MAddress expected = region->GetRegionStart();
    region->SetRegionAllocPtr(from + objectSize);
    routeDestination->SetRegionAllocPtr(routeDestination->GetRegionStart());
    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = productSpace.GetRegionManager();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    buffer->SetRegion(routeDestination);
    GC_EXPECT_TRUE(manager.RouteRegion(region));
    GC_EXPECT_TRUE(region->GetRouteState() == RegionInfo::RouteState::ROUTED);
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);

    const auto seeded = queue.Add(region, from);
    if (ZForwarding* forwarding = region->PeekForwardingOwner()) {
        forwarding->in_place_relocation_claim_page();
    }
    PageWaitEnterBarrier::Reset();
    RelocationRequestQueue::SetWaitEnterHook(&PageWaitEnterBarrier::Hook);
    BaseObject* resolved = nullptr;
    std::thread waiter([&]() {
        resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, liveObject, nullptr, region);
    });
    PageWaitEnterBarrier::WaitEntered();
    manager.ForwardFromRegions<Generation::Old>();
    RelocationRequestQueue::SetWaitEnterHook(nullptr);
    const auto claimed = seeded.request;
    BaseObject* workerResult = reinterpret_cast<BaseObject*>(ForwardingTable::FindTo(from));
    const bool workerClosed = queue.PendingCount() == 0;
    waiter.join();
    buffer->ClearRegion();

    GC_EXPECT_TRUE(resolved != nullptr);
    GC_EXPECT_TRUE(resolved != liveObject);
    GC_EXPECT_TRUE(seeded.accepted);
    GC_EXPECT_TRUE(claimed != nullptr);
    GC_EXPECT_TRUE(resolved == workerResult);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), reinterpret_cast<MAddress>(resolved));
    GC_EXPECT_TRUE(workerClosed);

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    routeDestination->SetRouteDestHold(0);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(ForwardingPublicationProduct, CompletedPageResolvesThroughForwardingTable)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* routeDestination =
        RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && routeDestination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* fromObject = fx.PlaceObject(region->GetRegionStart() + 64);
    const MAddress from = reinterpret_cast<MAddress>(fromObject);
    // The page task walks complete page layout; keep the unmarked prefix
    // walkable rather than relying on the old one-object-only test driver.
    for (MAddress at = region->GetRegionStart(); at < from;) {
        BaseObject* dead = fx.PlaceObject(at);
        at += RegionSpace::GetAllocSize(*dead);
    }
    region->SetRegionAllocPtr(from + fromObject->GetSize());
    routeDestination->SetRegionAllocPtr(routeDestination->GetRegionStart());

    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = productSpace.GetRegionManager();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    buffer->SetRegion(routeDestination);
    GC_EXPECT_TRUE(manager.RouteRegion(region));
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);

    const auto seeded = queue.Add(region, from);
    if (ZForwarding* forwarding = region->PeekForwardingOwner()) {
        forwarding->in_place_relocation_claim_page();
    }
    PageWaitEnterBarrier::Reset();
    RelocationRequestQueue::SetWaitEnterHook(&PageWaitEnterBarrier::Hook);
    BaseObject* resolved = nullptr;
    std::thread waiter([&]() {
        resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, fromObject, nullptr, region);
    });
    PageWaitEnterBarrier::WaitEntered();
    manager.ForwardFromRegions<Generation::Old>();
    RelocationRequestQueue::SetWaitEnterHook(nullptr);
    const auto claimed = seeded.request;
    BaseObject* workerResult = reinterpret_cast<BaseObject*>(ForwardingTable::FindTo(from));
    const bool workerClosed = queue.PendingCount() == 0;
    waiter.join();
    buffer->ClearRegion();

    const bool resolvedExpected = resolved != nullptr;
    const bool resolvedMoved = resolved != fromObject;
    const bool requestAccepted = seeded.accepted;
    const bool requestClaimed = claimed != nullptr;
    const bool workerMatched = resolved == workerResult;
    const bool tablePublished = ForwardingTable::FindTo(from) == reinterpret_cast<MAddress>(resolved);
    const bool generationClosed = workerClosed;

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    routeDestination->SetRouteDestHold(0);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);

    // Keep fault-injection failures after all product/global cleanup. The test
    // harness reports assertions with exceptions; throwing before this point
    // would contaminate later publication cases and turn one cut into rc=134.
    GC_EXPECT_TRUE(resolvedExpected);
    GC_EXPECT_TRUE(resolvedMoved);
    GC_EXPECT_TRUE(requestAccepted);
    GC_EXPECT_TRUE(requestClaimed);
    GC_EXPECT_TRUE(workerMatched);
    GC_EXPECT_TRUE(tablePublished);
    GC_EXPECT_TRUE(generationClosed);
}

// Product compact-request entry: the request is registered before compaction;
// CompactRegion itself copies the live second object, inserts its receipt, then
// zeroes that from slot.  The resolver must therefore answer the installed to,
// never the cleared from address.
GC_TEST(ForwardingPublicationProduct, CompactRequestReturnsReceiptBeforeFromClear)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = fx.region0;
    const MAddress start = region->GetRegionStart();
    BaseObject* dead = fx.PlaceObject(start);
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(start + objectSize);
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + objectSize);

    RegionManager manager;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RelocationReceiptTestAccess::ParkFrom(manager, region);

    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(region, from);
    GC_EXPECT_TRUE(request.accepted);

    manager.CompactRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());

    (void)queue.Wait(request.request);
    const MAddress resolved = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(resolved, start);
    GC_EXPECT_TRUE(resolved != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), resolved);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(resolved)->IsValidObject());

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ClearEntries(start, region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(ForwardingPublicationProduct, CompactInsertSurvivesVerifyClearAndReclaim)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + objectSize);
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + objectSize);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    GC_EXPECT_TRUE(region->IsForwardingDone());

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");

    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_EQ(lookup.to, to);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::Unavailable);

    FindToVersionResult found = RelocationReceiptTestAccess::ProductFindToVersion(collector, liveObject);
    GC_EXPECT_TRUE(found.state() == FindToVersionResult::State::Found);
    GC_EXPECT_TRUE(found.found() == reinterpret_cast<BaseObject*>(to));

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, InsertThenReclaimStillServesWaitAndTryUpdate)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(publication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(to)).address,
                 reinterpret_cast<MAddress>(to));
    publication = ForwardingTable::Publication();
    from->SetStateCode(ObjectState::FORWARDED);
    region->MarkForwardingDone();
    region->MarkForwardingDone();
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from)).to,
                 reinterpret_cast<MAddress>(to));

    BaseObject* waited = RelocationReceiptTestAccess::WaitRoutedTipReady(
        collector, from, nullptr, region);
    GC_EXPECT_TRUE(waited == to);

    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & REMAP_COLOUR_MASK;
    if (staleRemaps != 0) {
        RefField<> field(to_zpointer(reinterpret_cast<uintptr_t>(from) | staleRemaps));
        BaseObject* updated = nullptr;
        (void)RelocationReceiptTestAccess::TryUpdateRefField(collector, nullptr, field, updated);
        if (updated != nullptr) {
            GC_EXPECT_TRUE(updated == to);
        }
    }

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    RelocationReceiptTestAccess::ReleaseListOwnership(destination);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, PostRemapResetDestroysAfterA8Coverage)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    const size_t objectSize = dead->GetSize();
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + objectSize);
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + objectSize);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* youngTab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(youngTab != nullptr);
    GC_EXPECT_EQ(youngTab->table_generation(), static_cast<uint8_t>(Generation::Young));

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::ArmedHit);

    PublishGenerationMarkComplete(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    DestroyAfterGhostCleared(region, "young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    GC_EXPECT_TRUE((static_cast<uint8_t>(ForwardingTable::LookupTo(from).unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::TableDestroyed)) != 0);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, RetiredYoungTableSurvivesA8UntilNextYoungMarkCoverage)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* youngTab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(youngTab != nullptr);
    GC_EXPECT_EQ(youngTab->table_generation(), static_cast<uint8_t>(Generation::Young));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    FindToVersionResult found = RelocationReceiptTestAccess::ProductFindToVersion(collector, liveObject);
    GC_EXPECT_TRUE(found.state() == FindToVersionResult::State::Found);
    GC_EXPECT_TRUE(found.found() == reinterpret_cast<BaseObject*>(to));
    PublishGenerationMarkComplete(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    DestroyAfterGhostCleared(region, "young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, RetiredOldTableNotFreedByYoungCoverage)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Old);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* tab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_EQ(tab->table_generation(), static_cast<uint8_t>(Generation::Old));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::ArmedHit);
    PublishGenerationMarkComplete(Generation::Old);
    ForwardingTable::ReclaimRetired("old-mark-coverage");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    DestroyAfterGhostCleared(region, "old-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, RetiredYoungTableNotFreedByOldCoverage)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* tab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_EQ(tab->table_generation(), static_cast<uint8_t>(Generation::Young));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Old);
    ForwardingTable::ReclaimRetired("old-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::ArmedHit);
    PublishGenerationMarkComplete(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    DestroyAfterGhostCleared(region, "young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, HeldLookupReaderDefersEligibleDestroy)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* youngTab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(youngTab != nullptr);
    GC_EXPECT_EQ(youngTab->table_generation(), static_cast<uint8_t>(Generation::Young));
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    {
        ForwardingTable::Publication reader = ForwardingTable::RetainCovering(from);
        GC_EXPECT_TRUE(static_cast<bool>(reader));
        ForwardingTable::ReclaimRetired("young-mark-coverage");
        GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    }
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    DestroyAfterGhostCleared(region, "young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, CoverageEpochAdvancesOnlyAtMarkEnd)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    ZForwarding* tab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_EQ(tab->table_generation(), static_cast<uint8_t>(Generation::Young));
    const uint64_t required = tab->required_mark_epoch();
    const GCCycleSnapshot before =
        Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG);
    GC_EXPECT_TRUE(before.phase != GC_PHASE_MARK_COMPLETE);
    GC_EXPECT_TRUE(before.sequence < required || !((before.phase == GC_PHASE_MARK_COMPLETE) ||
                                                   (before.phase == GC_PHASE_POST_TRACE)));
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    const GCCycleSnapshot afterA8 =
        Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG);
    GC_EXPECT_EQ(afterA8.sequence, before.sequence);
    GC_EXPECT_EQ(afterA8.phase, before.phase);
    PublishGenerationMarkComplete(Generation::Young);
    const GCCycleSnapshot after =
        Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG);
    GC_EXPECT_EQ(after.phase, GC_PHASE_MARK_COMPLETE);
    GC_EXPECT_TRUE(after.sequence >= required || after.phase == GC_PHASE_MARK_COMPLETE);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ResolveStoreValueNoForwardingAfterGhostDispelLogs)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ZForwarding* tab = ForwardingTable::GetCovering(from);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_EQ(tab->table_generation(), static_cast<uint8_t>(Generation::Young));

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    DestroyAfterGhostCleared(region, "young-mark-coverage");
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(from) == nullptr);

    __atomic_store_n(reinterpret_cast<uint64_t*>(from), 0, __ATOMIC_RELAXED);
#if defined(__linux__)
    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture aborted = CaptureAbort([&]() { barrier.WriteReference(holder, field, liveObject); });
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
    const char* required[] = {
        "[LOADFC][fail-closed] site=WCollector::ResolveStoreValue.no-forwarding",
        "holder_kind=heap_ref",
        "holder=",
        "slot=",
        "from=",
        "from_region=",
        "table_id=",
        "lookup_state=",
        "lookup_cause=",
        "retired_lookup=",
        "gc_phase=",
    };
    for (const char* token : required) {
        GC_EXPECT_TRUE(aborted.output.find(token) != std::string::npos);
    }
    char holderToken[64] {};
    char slotToken[64] {};
    (void)std::snprintf(holderToken, sizeof(holderToken), "holder=%p", holder);
    (void)std::snprintf(slotToken, sizeof(slotToken), "slot=%p", &field);
    GC_EXPECT_TRUE(aborted.output.find(holderToken) != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find(slotToken) != std::string::npos);
    GC_EXPECT_TRUE(aborted.output.find("holder_kind=unknown") == std::string::npos);
#endif
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// ClearEntries must seal an installed table and wait for the publication owner
// that crossed the copy boundary.  The owner inserts while clear is waiting;
// only after the owner releases may clear unlink and retire the table.
GC_TEST(ForwardingPublicationProduct, ClearWaitsForHeldPublicationAndKeepsReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(1, 2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* fromObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(fromObject) + fromObject->GetSize());
    const MAddress from = reinterpret_cast<MAddress>(fromObject);
    const MAddress to = fx.region0->GetRegionStart();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(region, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    ZForwarding* heldTable = ForwardingTable::GetEntries(region->GetRegionStart());
    GC_EXPECT_TRUE(heldTable != nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart() + RegionInfo::UNIT_SIZE) != nullptr);

    std::atomic<bool> clearStarted{ false };
    std::atomic<bool> clearDone{ false };
    std::thread clearer([&]() {
        clearStarted.store(true, std::memory_order_release);
        ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
        clearDone.store(true, std::memory_order_release);
    });
    while (!clearStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (heldTable != nullptr && !heldTable->table_draining()) {
        std::this_thread::yield();
    }
    const bool clearWaitedForPublication = !clearDone.load(std::memory_order_acquire);

    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, to);
    GC_EXPECT_TRUE(receipt.installed);
    GC_EXPECT_EQ(receipt.address, to);
    publication = ForwardingTable::Publication();
    clearer.join();

    GC_EXPECT_TRUE(clearWaitedForPublication);
    GC_EXPECT_TRUE(clearDone.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart() + RegionInfo::UNIT_SIZE) == nullptr);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(from), to);

    // Page relocation and forwarding-metadata retirement are independent.
    // Reusing the source page must neither wait for coverage nor invalidate the
    // retired receipt (zRelocate.cpp:1041-1047; zRelocationSet.cpp:191-197).
    const auto detachSite = FromPageDetach::Site::INIT_REGION_INFO;
    const FromPageDetach::Counters beforeReuse = FromPageDetach::GetCounters(detachSite);
    GC_EXPECT_TRUE(FromPageDetach::FromPageDetachCheck(region, detachSite));
    const FromPageDetach::Counters afterPrecheck = FromPageDetach::GetCounters(detachSite);
    GC_EXPECT_EQ(afterPrecheck.retiredTable, beforeReuse.retiredTable + 1);
    GC_EXPECT_EQ(afterPrecheck.withEvidence, beforeReuse.withEvidence);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    const RegionLifeId oldLife = region->GetRegionLifeId();
    region->InitRegion(region->GetUnitCount(), RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    GC_EXPECT_NE(region->GetRegionLifeId(), oldLife);
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(from), to);

    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(from), static_cast<MAddress>(0));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// zRelocationSet.cpp:191-197: clearing the table waits for outstanding table
// users independently of the source-page count. Observe drain admission and
// the held publication token before allowing the publisher to complete.
GC_TEST(ForwardingPublicationProduct, ClearDrainEntersClaimedWaitBeforeReturning)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(1, 2, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* fromObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(fromObject) + fromObject->GetSize());
    const MAddress from = reinterpret_cast<MAddress>(fromObject);
    const MAddress to = fx.region0->GetRegionStart();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(region, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    ZForwarding* heldTable = ForwardingTable::GetEntries(region->GetRegionStart());
    GC_EXPECT_TRUE(heldTable != nullptr);

    std::atomic<bool> clearDone{ false };
    std::thread clearer([&]() {
        ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
        clearDone.store(true, std::memory_order_release);
    });
    while (heldTable != nullptr && !heldTable->table_draining() &&
           !clearDone.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool enteredClaimedDrain =
        heldTable != nullptr && heldTable->table_draining() && heldTable->table_readers() != 0;

    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, to);
    GC_EXPECT_TRUE(receipt.installed);
    GC_EXPECT_EQ(receipt.address, to);
    publication = ForwardingTable::Publication();
    clearer.join();

    GC_EXPECT_TRUE(enteredClaimedDrain);
    GC_EXPECT_TRUE(clearDone.load(std::memory_order_acquire));
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(from), to);

    ForwardingTable::Publication late = ForwardingTable::EnsurePublicationBeforeCopy(region, from);
    const bool reopenedAfterClear = static_cast<bool>(late);
    GC_EXPECT_FALSE(reopenedAfterClear);
    late = ForwardingTable::Publication();
    if (reopenedAfterClear) {
        // Keep the fault arm isolated: release the late owner, then let a second
        // clear close and retire the accidentally reopened table.
        ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    }

    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// zRelocate.cpp:362-372: Exclusive owns the before-copy Publication through
// CopyObject, receipt installation, queue publication and FORWARDED state.  Use
// the product allocator's real queue so no receipt is hand-fed by this test.
GC_TEST(ForwardingPublicationProduct, ExclusiveCopyPublishesProductReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* fromObject = fx.PlaceObject(region->GetRegionStart() + 64);
    BaseObject* toObject = fx.PlaceObject(destination->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(fromObject) + fromObject->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(toObject) + toObject->GetSize());
    const MAddress from = reinterpret_cast<MAddress>(fromObject);
    const MAddress to = reinterpret_cast<MAddress>(toObject);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);

    StateWord oldWord = fromObject->GetStateWord();
    GC_EXPECT_TRUE(fromObject->TryLockObject(oldWord));
    GC_EXPECT_TRUE(true);
    BaseObject* relocated =
        RelocationReceiptTestAccess::ForwardExclusive(collector, fromObject, toObject, region);

    const bool productPublished = ForwardingTable::FindTo(from) != 0;
    GC_EXPECT_TRUE(productPublished);
    GC_EXPECT_TRUE(relocated == toObject);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), to);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), to);
    GC_EXPECT_TRUE(fromObject->IsForwarded());
    GC_EXPECT_EQ(region->metadata.copyInflight.load(std::memory_order_acquire), 0);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}


// ZGC zRelocate.cpp:1256-1279: the promoted page keeps the relocation-set
// livemap selected at registration, and discharge walks only that live set.
GC_TEST(LoadHealDeliveryProduct, DualCarrierProducerCapturesOldTopAndLivemap)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = ResetDeliveryUnit(fx, 0);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + liveObject->GetSize());
    const MAddress oldTop = region->GetRegionAllocPtr();
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(liveObject));

    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    const ZForwarding::FromPageView* from = ForwardingTable::GetFromPageView(region);
    GC_EXPECT_TRUE(from != nullptr);
    GC_EXPECT_EQ(from == nullptr ? 0 : from->topAtStart, oldTop);
    GC_EXPECT_TRUE(from != nullptr && from->liveInfo == live);
    GC_EXPECT_TRUE(from != nullptr && from->epoch != 0);
    GC_EXPECT_TRUE(region->IsOwnerSurvivedObject(offset));

    region->DispelGhostFromRegion();
    ForwardingTable::ReclaimRetired("gc-unit-dual-carrier-producer");
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// zForwarding.cpp:55-84 / zRelocate.cpp:871-877: resetting the to-page
// allocation top must not retarget the from-page iteration view. The consumer
// keeps using the forwarding carrier until Dispel retires it.
GC_TEST(LoadHealDeliveryProduct, DualCarrierConsumerSurvivesCurrentPageResetUntilRetire)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = ResetDeliveryUnit(fx, 0);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + liveObject->GetSize());
    const MAddress oldTop = region->GetRegionAllocPtr();
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(liveObject));

    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    region->SetRegionAllocPtr(region->GetRegionStart());
    region->metadata.liveInfo = nullptr;

    const ZForwarding::FromPageView* from = ForwardingTable::GetFromPageView(region);
    GC_EXPECT_TRUE(from != nullptr);
    GC_EXPECT_EQ(from == nullptr ? 0 : from->topAtStart, oldTop);
    GC_EXPECT_TRUE(region->IsOwnerSurvivedObject(offset));

    region->DispelGhostFromRegion();
    GC_EXPECT_TRUE(ForwardingTable::GetFromPageView(region) == nullptr);
    GC_EXPECT_FALSE(region->IsOwnerSurvivedObject(offset));
    ForwardingTable::ReclaimRetired("gc-unit-dual-carrier-consumer");
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    fx.FreePlanted(live);
}

// ZGC zRelocate.cpp:1256-1279: the promoted page keeps the relocation-set
// livemap selected at registration, and discharge walks only that live set.
GC_TEST(LoadHealDeliveryProduct, PromotedSnapshotDischargesOnlyLiveHolder)
{
    GcHeapFixture& fx = ProductFixture();
    PromotedRegionDomain::ResetForNextMinor(100);
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* targetRegion = ResetDeliveryUnit(fx, 1);
    holderRegion->SetYoungRegionFlag(1);
    targetRegion->SetYoungRegionFlag(1);
    targetRegion->SetYoungAge(1);

    const MAddress holderStart = holderRegion->GetRegionStart();
    BaseObject* liveHolder = fx.PlaceObject(holderStart);
    const size_t objectSize = liveHolder->GetSize();
    BaseObject* deadHolder = fx.PlaceObject(holderStart + objectSize);
    BaseObject* youngTarget = fx.PlaceObject(targetRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(deadHolder) + objectSize);
    targetRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngTarget) + youngTarget->GetSize());

    auto* liveField = &HeapSlotAt<>(reinterpret_cast<MAddress>(liveHolder) + TYPEINFO_PTR_SIZE);
    auto* deadField = &HeapSlotAt<>(reinterpret_cast<MAddress>(deadHolder) + TYPEINFO_PTR_SIZE);
    liveField->StoreColoured(GcUnit::StoreGoodPointer(youngTarget));
    deadField->StoreColoured(GcUnit::StoreGoodPointer(youngTarget));

    LiveInfo* live = fx.PlantLiveInfo(holderRegion);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, holderRegion->GetRegionSize());
    (void)bitmap->MarkBits(0, objectSize, holderRegion->GetRegionSize());
    holderRegion->AddLiveByteCount(objectSize);
    PromotedRegionDomain::Register(holderRegion, PromotedRegionDomain::RegisterPath::InPlace);

    // Registration is the producer boundary.  Remove the current face before
    // discharge so only Entry::CopyMarkWordsForView can carry the decision.
    holderRegion->metadata.liveInfo = nullptr;
    holderRegion->SetYoungRegionFlag(0);
    size_t liveResolve = 0;
    size_t deadResolve = 0;
    std::unordered_set<MAddress> recordedSlots;
    const size_t recorded = PromotedRegionDomain::DischargeAll(
        [&](RefField<>& field) -> BaseObject* {
            const MAddress slot = reinterpret_cast<MAddress>(&field);
            liveResolve += slot == reinterpret_cast<MAddress>(liveField) ? 1 : 0;
            deadResolve += slot == reinterpret_cast<MAddress>(deadField) ? 1 : 0;
            return to_object(field.GetTargetObject());
        },
        [&](MAddress slot) { recordedSlots.insert(slot); });

    std::fprintf(stderr,
                 "DETAIL loadheal_promoted registered=%zu recorded=%zu live_resolve=%zu "
                 "dead_resolve=%zu live_slot=%zu dead_slot=%zu\n",
                 PromotedRegionDomain::RegisteredCount(), recorded, liveResolve, deadResolve,
                 recordedSlots.count(reinterpret_cast<MAddress>(liveField)),
                 recordedSlots.count(reinterpret_cast<MAddress>(deadField)));
    std::fflush(stderr);
    GC_EXPECT_EQ(recorded, 1u);
    GC_EXPECT_EQ(liveResolve, 1u);
    GC_EXPECT_EQ(deadResolve, 0u);
    GC_EXPECT_EQ(recordedSlots.count(reinterpret_cast<MAddress>(liveField)), 1u);
    GC_EXPECT_EQ(recordedSlots.count(reinterpret_cast<MAddress>(deadField)), 0u);

    PromotedRegionDomain::ResetForNextMinor(101);
    fx.FreePlanted(live);
    targetRegion->SetYoungRegionFlag(0);
}

// ZGC zRelocate.cpp:652-731,838-861: lift the old page face before reuse,
// move a field bit with its object, then prove the real minor consumer reaches
// the young target through the moved slot.
GC_TEST(LoadHealDeliveryProduct, InPlaceRemsetMovesBitAndFeedsConsumer)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* targetRegion = ResetDeliveryUnit(fx, 1);
    targetRegion->SetYoungRegionFlag(1);
    targetRegion->SetYoungAge(1);

    BaseObject* from = fx.PlaceObject(holderRegion->GetRegionStart());
    const size_t objectSize = from->GetSize();
    BaseObject* to = fx.PlaceObject(reinterpret_cast<MAddress>(from) + objectSize);
    BaseObject* youngTarget = fx.PlaceObject(targetRegion->GetRegionStart());
    LiveInfo* targetLive = fx.PlantLiveInfo(targetRegion);
    (void)fx.PlantMarkBitmap<Generation::Young>(targetLive, targetRegion->GetRegionSize());
    fx.typeInfo->SetUUID(1);
    TypeInfoManager::GetTypeInfoManager().AddTypeInfo(fx.typeInfo);
    GC_EXPECT_TRUE(TypeInfoManager::GetTypeInfoManager().ContainsTypeInfo(fx.typeInfo));
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + objectSize);
    targetRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngTarget) + youngTarget->GetSize());
    auto* fromField = &HeapSlotAt<>(reinterpret_cast<MAddress>(from) + TYPEINFO_PTR_SIZE);
    const MAddress oldSlot = reinterpret_cast<MAddress>(fromField);
    const MAddress newSlot = reinterpret_cast<MAddress>(to) + TYPEINFO_PTR_SIZE;

    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LoadHealDeliveryTestAccess::PublishColours(collector);
    Barrier barrier(collector, remembered);
    {
        DeliveryNoAllocBufferScope directRemember;
        fromField->StoreColoured(zpointer::null);
        barrier.WriteReference(from, *fromField, youngTarget);
    }
    GC_EXPECT_TRUE(remembered.Contains(oldSlot));

    std::vector<RememberedSet::InPlaceSlot> takenSlots;
    const size_t taken = remembered.TakeInPlaceSlots(holderRegion->GetRegionStart(),
                                                     holderRegion->GetRegionEnd(), takenSlots);
    const bool oldCleared = !remembered.Contains(oldSlot);
    std::memcpy(to, from, objectSize);
    const size_t moved = remembered.MoveInPlaceSlots(
        takenSlots, reinterpret_cast<MAddress>(from), reinterpret_cast<MAddress>(to), objectSize);
    const bool newPresent = remembered.Contains(newSlot);

    std::unordered_set<MAddress> previous;
    remembered.DrainForMinor(previous);
    const LoadHealDeliveryTestAccess::RemsetConsumeResult consumed =
        LoadHealDeliveryTestAccess::ConsumeRemembered(collector, previous, to);
    std::fprintf(stderr,
                 "DETAIL loadheal_inplace taken=%zu old_cleared=%u moved=%zu new_present=%u "
                 "previous_new=%zu consumer_consumed=%zu consumer_work=%zu\n",
                 taken, static_cast<unsigned>(oldCleared), moved, static_cast<unsigned>(newPresent),
                 previous.count(newSlot), consumed.consumed, consumed.work);
    std::fflush(stderr);

    // Producer and consumer have disjoint criteria: Take owns taken/old-cleared;
    // Move owns new-present and the product rescan reachability result.
    GC_EXPECT_EQ(taken, 1u);
    GC_EXPECT_TRUE(oldCleared);
    GC_EXPECT_EQ(moved, 1u);
    GC_EXPECT_TRUE(newPresent);
    GC_EXPECT_EQ(previous.count(newSlot), 1u);
    GC_EXPECT_EQ(consumed.consumed, 1u);
    GC_EXPECT_EQ(consumed.work, 1u);
    EmptyBothRememberedFaces(remembered);
    targetRegion->metadata.liveInfo = nullptr;
    fx.FreePlanted(targetLive);
    targetRegion->SetYoungRegionFlag(0);
}

// The conservative pinned producer is accepted only for a value inside the
// young page's current [start, allocPtr) domain.
GC_TEST(LoadHealDeliveryProduct, CrossGenRangeGateRecordsLegalAndRejectsBeyondTop)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* targetRegion = ResetDeliveryUnit(fx, 1);
    targetRegion->SetYoungRegionFlag(1);
    targetRegion->SetYoungAge(1);

    BaseObject* legalHolder = fx.PlaceObject(holderRegion->GetRegionStart());
    const size_t objectSize = legalHolder->GetSize();
    BaseObject* invalidHolder = fx.PlaceObject(reinterpret_cast<MAddress>(legalHolder) + objectSize);
    BaseObject* legalTarget = fx.PlaceObject(targetRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(invalidHolder) + objectSize);
    targetRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(legalTarget) + legalTarget->GetSize());
    BaseObject* beyondTop = reinterpret_cast<BaseObject*>(targetRegion->GetRegionAllocPtr() + 64);
    auto* legalField = &HeapSlotAt<>(reinterpret_cast<MAddress>(legalHolder) + TYPEINFO_PTR_SIZE);
    auto* invalidField = &HeapSlotAt<>(reinterpret_cast<MAddress>(invalidHolder) + TYPEINFO_PTR_SIZE);
    legalField->StoreColoured(GcUnit::StoreGoodPointer(legalTarget));
    invalidField->StoreColoured(GcUnit::StoreGoodPointer(beyondTop));

    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    RegionManager manager;
    manager.EnlistFullThreadLocalRegion(holderRegion);
    RuntimeWorkers runtimeWorkers(2);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &runtimeWorkers);
    const size_t recorded = manager.RecordPinnedCrossGenEdges();
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
    const std::unordered_set<MAddress> snapshot = remembered.Snapshot();
    const size_t legalRecorded = snapshot.count(reinterpret_cast<MAddress>(legalField));
    const size_t invalidRecorded = snapshot.count(reinterpret_cast<MAddress>(invalidField));
    std::fprintf(stderr,
                 "DETAIL loadheal_crossgen producer_recorded=%zu legal_recorded=%zu "
                 "invalid_recorded=%zu target_top=0x%zx invalid_target=0x%zx\n",
                 recorded, legalRecorded, invalidRecorded,
                 static_cast<size_t>(targetRegion->GetRegionAllocPtr()),
                 reinterpret_cast<size_t>(beyondTop));
    std::fflush(stderr);

    GC_EXPECT_EQ(recorded, 1u);
    GC_EXPECT_EQ(legalRecorded, 1u);
    GC_EXPECT_EQ(invalidRecorded, 0u);
    RelocationReceiptTestAccess::ReleaseListOwnership(holderRegion);
    EmptyBothRememberedFaces(remembered);
    targetRegion->SetYoungRegionFlag(0);
}

// Direct semantic matrix for the current remembered face. The reference array
// is live, but its far field lies beyond TryRecoverInteriorBase's 64-byte
// recovery window. ZGC still applies the load barrier because the current old
// page, rather than an object-level recovery guess, is the admission unit.
GC_TEST(LoadHealDeliveryProduct, CurrentRemsetRemapsLiveRemoteArrayField)
{
    LoadHealDeliveryRuntime::Ensure();
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* youngRegion = ResetDeliveryUnit(fx, 1);
    RegionInfo* youngCarrier = ResetDeliveryUnit(fx, 3);
    youngRegion->SetYoungRegionFlag(1);
    youngRegion->SetYoungAge(1);

    DeliveryReferenceArrayTypes& types = GetDeliveryReferenceArrayTypes();
    auto* holder = reinterpret_cast<MArray*>(holderRegion->GetRegionStart());
    holder->SetClassInfo(types.array);
    holder->SetLength(16);
    auto* youngHolder = reinterpret_cast<MArray*>(youngCarrier->GetRegionStart());
    youngHolder->SetClassInfo(types.array);
    youngHolder->SetLength(16);
    BaseObject* youngTarget = fx.PlaceObject(youngRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(holder) + holder->GetMArraySize());
    youngCarrier->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngHolder) + youngHolder->GetMArraySize());
    youngRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngTarget) + youngTarget->GetSize());
    // Snapshot after the holder allocation so it is not covered by the
    // allocate-black mark-start gap. Only its object-head live bit applies.
    holderRegion->ClearLiveInfo(holderRegion->GetMarkView<Generation::Old>());
    auto* nearField = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + MArray::GetContentOffset());
    for (size_t i = 0; i < 16; ++i) {
        HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + MArray::GetContentOffset() + i * sizeof(void*))
            .StoreColoured(zpointer::null);
    }
    auto* farField = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + MArray::GetContentOffset() + 15 * sizeof(void*));
    auto* youngField = &HeapSlotAt<>(reinterpret_cast<MAddress>(youngHolder) + MArray::GetContentOffset());
    const MAddress nearSlot = reinterpret_cast<MAddress>(nearField);
    const MAddress farSlot = reinterpret_cast<MAddress>(farField);
    const MAddress youngSlot = reinterpret_cast<MAddress>(youngField);
    const size_t farOffset = farSlot - reinterpret_cast<MAddress>(holder);

    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LoadHealDeliveryTestAccess::PublishColours(collector);
    Barrier barrier(collector, remembered);
    {
        DeliveryNoAllocBufferScope directRemember;
        nearField->StoreColoured(zpointer::null);
        farField->StoreColoured(zpointer::null);
        youngField->StoreColoured(zpointer::null);
        barrier.WriteReference(holder, *nearField, youngTarget);
        barrier.WriteReference(holder, *farField, youngTarget);
        barrier.WriteReference(youngHolder, *youngField, youngTarget);
    }
    GC_EXPECT_TRUE(remembered.Contains(nearSlot));
    GC_EXPECT_TRUE(remembered.Contains(farSlot));
    GC_EXPECT_TRUE(remembered.Contains(youngSlot));

    LiveInfo* holderLive = fx.PlantLiveInfo(holderRegion);
    RegionBitmap* holderMarks = fx.PlantMarkBitmap<Generation::Old>(holderLive, holderRegion->GetRegionSize());
    (void)holderMarks->MarkBits(0, holder->GetMArraySize(), holderRegion->GetRegionSize());
    holderRegion->AddLiveByteCount(holder->GetMArraySize());
    holderRegion->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    youngCarrier->SetYoungRegionFlag(1);

    LateBackfillState forwarding = PrepareLateBackfill(fx, collector);
    nearField->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    farField->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    youngField->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    const uintptr_t doubleBad = LoadHealDeliveryTestAccess::DoubleBadColour(collector);
    GC_EXPECT_TRUE(doubleBad != 0 && (doubleBad & (doubleBad - 1)) == 0);
    GC_EXPECT_EQ(raw(farField->GetFieldValue()) & REMAP_COLOUR_MASK, doubleBad);
    const uintptr_t youngBefore = raw(youngField->GetFieldValue());
    LoadHealDeliveryTestAccess::RemapYoungRoots(collector);

    const bool nearResolved = to_object(nearField->GetTargetObject()) == forwarding.to;
    const bool farResolved = to_object(farField->GetTargetObject()) == forwarding.to;
    const bool nearStoreGood = collector.is_store_good(*nearField);
    const bool farStoreGood = collector.is_store_good(*farField);
    const bool youngUnchanged = raw(youngField->GetFieldValue()) == youngBefore;
    const bool holderNonAllocating = !holderRegion->HasMarkStartAllocGap();
    const bool matrixResult = farOffset > 64 && holderNonAllocating && nearResolved && farResolved &&
        nearStoreGood && farStoreGood && youngUnchanged;
    std::fprintf(stderr,
                 "DETAIL current_remset_matrix far_offset=%zu holder_live=%u holder_nonalloc=%u near_resolved=%u "
                 "far_resolved=%u near_store_good=%u far_store_good=%u young_unchanged=%u result=%u\n",
                 farOffset, static_cast<unsigned>(holderRegion->IsMarkedObject(
                     holderRegion->GetMarkView<Generation::Old>(), holder)),
                 static_cast<unsigned>(holderNonAllocating),
                 static_cast<unsigned>(nearResolved), static_cast<unsigned>(farResolved),
                 static_cast<unsigned>(nearStoreGood), static_cast<unsigned>(farStoreGood),
                 static_cast<unsigned>(youngUnchanged), static_cast<unsigned>(matrixResult));
    std::fflush(stderr);

    GC_EXPECT_TRUE(matrixResult);
    nearField->StoreColoured(zpointer::null);
    farField->StoreColoured(zpointer::null);
    youngField->StoreColoured(zpointer::null);
    EmptyBothRememberedFaces(remembered);
    LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    CleanupLateBackfill(fx, forwarding);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    holderRegion->metadata.liveInfo = nullptr;
    fx.FreePlanted(holderLive);
    youngCarrier->SetYoungRegionFlag(0);
    youngRegion->SetYoungRegionFlag(0);
}

// True runtime entry: this test never calls RemapYoungRoots or Preforward. It
// enters at DoGarbageCollection, then reads the one-shot receipt sampled by the
// product remap loop before relocate-start flips the colour masks.
#if defined(MRT_REMAP_YOUNG_ROOTS_RECEIPT_AVAILABLE)
GC_OTHER_VM_TEST(LoadHealDeliveryProduct, MajorDispatchRemapsLiveRemoteArrayField)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    LoadHealDeliveryRuntime::Ensure();
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* youngRegion = ResetDeliveryUnit(fx, 1);
    youngRegion->SetYoungRegionFlag(1);
    youngRegion->SetYoungAge(1);

    DeliveryReferenceArrayTypes& types = GetDeliveryReferenceArrayTypes();
    auto* holder = reinterpret_cast<MArray*>(holderRegion->GetRegionStart());
    holder->SetClassInfo(types.array);
    holder->SetLength(16);
    BaseObject* youngTarget = fx.PlaceObject(youngRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(holder) + holder->GetMArraySize());
    youngRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngTarget) + youngTarget->GetSize());
    holderRegion->ClearLiveInfo(holderRegion->GetMarkView<Generation::Old>());
    for (size_t i = 0; i < 16; ++i) {
        HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + MArray::GetContentOffset() + i * sizeof(void*))
            .StoreColoured(zpointer::null);
    }
    auto* farField = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + MArray::GetContentOffset() + 15 * sizeof(void*));
    const MAddress farSlot = reinterpret_cast<MAddress>(farField);
    const size_t farOffset = farSlot - reinterpret_cast<MAddress>(holder);

    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    LoadHealDeliveryTestAccess::PublishColours(collector);
    Barrier barrier(collector, remembered);
    {
        DeliveryNoAllocBufferScope directRemember;
        farField->StoreColoured(zpointer::null);
        barrier.WriteReference(holder, *farField, youngTarget);
    }
    GC_EXPECT_TRUE(remembered.Contains(farSlot));

    LiveInfo* holderLive = fx.PlantLiveInfo(holderRegion);
    RegionBitmap* holderMarks = fx.PlantMarkBitmap<Generation::Old>(holderLive, holderRegion->GetRegionSize());
    (void)holderMarks->MarkBits(0, holder->GetMArraySize(), holderRegion->GetRegionSize());
    holderRegion->AddLiveByteCount(holder->GetMArraySize());
    holderRegion->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LateBackfillState forwarding = PrepareLateBackfill(fx, collector);
    farField->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    // Model the prior young relocate-start that makes a current old-remset
    // field load-bad. Major mark-start changes mark colours only; the true
    // Preforward entry must consume this remap-stale word.
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);

    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    ResetRemapYoungRootsTestReceipt(farSlot);

    collector.RunGarbageCollection(1, GC_REASON_USER);

    const RemapYoungRootsTestReceipt receipt = ReadRemapYoungRootsTestReceipt();
    const bool holderNonAllocating = !holderRegion->HasMarkStartAllocGap();
    const bool targetResult = receipt.visits == 1 && receipt.heals == 1 &&
        receipt.resolvedAddress == reinterpret_cast<uintptr_t>(forwarding.to) &&
        receipt.storeGoodAfter && receipt.before != receipt.after && farOffset > 64 && holderNonAllocating;
    std::fprintf(stderr,
                 "TARGET_CURRENT_REMSET_ASSERT_EXECUTED visits=%llu heals=%llu far_offset=%zu holder_nonalloc=%u "
                 "before=0x%zx after=0x%zx resolved=0x%zx expected=0x%zx store_good=%u result=%u\n",
                 static_cast<unsigned long long>(receipt.visits),
                 static_cast<unsigned long long>(receipt.heals), farOffset,
                 static_cast<unsigned>(holderNonAllocating),
                 static_cast<size_t>(receipt.before), static_cast<size_t>(receipt.after),
                 static_cast<size_t>(receipt.resolvedAddress), reinterpret_cast<size_t>(forwarding.to),
                 static_cast<unsigned>(receipt.storeGoodAfter), static_cast<unsigned>(targetResult));
    std::fflush(stderr);

    // Keep the existence diagnostics non-fatal: the single target invariant
    // below is reached in green, entry-cut, and holder-gate arms alike.
    GC_EXPECT_TRUE(targetResult);

    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
}
#endif

GC_TEST(ForwardingPublicationProduct, GhostHeldRetainsResolvableCarrier)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    // Force every release condition except the independent ghost-held guard.
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    const ForwardingTable::LookupResult kept = ForwardingTable::LookupTo(from);
    const bool ghostHeld = region->IsGhostFromRegion();
    const bool retiredCovers =
        ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize());
    DestroyAfterGhostCleared(region, "gc-unit-explicit-coverage");
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    // Keep the deliberate-red arm isolated: record product observations first,
    // always restore fixture state, then let the assertions report the cut.
    GC_EXPECT_TRUE(ghostHeld);
    GC_EXPECT_TRUE(retiredCovers);
    GC_EXPECT_EQ(kept.to, to);
    GC_EXPECT_TRUE(kept.answer == ForwardingTable::ToAnswer::ArmedHit);
}

GC_TEST(ForwardingPublicationProduct, GhostClearedAllowsEligibleCarrierReclaim)
{
    GcHeapFixture& fx = ProductFixture();
    ForwardingTable::ReclaimRetired("gc-unit-fixture-coverage-complete");
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = RegionInfo::InitRegion(4, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    PinOwnerGeneration(region, Generation::Young);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    const MAddress from = reinterpret_cast<MAddress>(liveObject);
    region->SetRegionAllocPtr(from + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    manager.CompactRegion(region);
    const MAddress to = ForwardingTable::FindTo(from);
    GC_EXPECT_TRUE(to != 0);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    PublishGenerationMarkComplete(Generation::Young);
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_TRUE(ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize()));

    region->DispelGhostFromRegion();
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");

    GC_EXPECT_FALSE(region->IsGhostFromRegion());
    GC_EXPECT_FALSE(ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize()));
    const ForwardingTable::LookupResult reclaimed = ForwardingTable::LookupTo(from);
    GC_EXPECT_TRUE(reclaimed.answer == ForwardingTable::ToAnswer::Unavailable);
    GC_EXPECT_TRUE((static_cast<uint8_t>(reclaimed.unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::TableDestroyed)) != 0);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}
