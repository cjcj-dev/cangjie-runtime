// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_cycle_sequence_fixture.hpp"
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
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
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
    template<Generation G>
    static void PrepareProductPage(RegionInfo* region)
    {
        using ProductFn = void (*)(RegionInfo*, MarkView<G>);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        const char* name = G == Generation::Young
            ? "_ZN12MapleRuntime10RegionInfo24PrepareForwardableRegionILNS_10GenerationE0EEEvNS_8MarkViewIXT_EEE"
            : "_ZN12MapleRuntime10RegionInfo24PrepareForwardableRegionILNS_10GenerationE1EEEvNS_8MarkViewIXT_EEE";
        void* symbol = dlsym(handle, name);
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        reinterpret_cast<ProductFn>(symbol)(region, region->GetMarkView<G>());
        dlclose(handle);
    }

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
        if (collector != nullptr && resources.collectorProxy.currentCollector != nullptr) {
            GcUnit::GcHeapFixture::AdoptGenerationIdentity(*collector, *resources.collectorProxy.currentCollector);
        }
        resources.collectorProxy.currentCollector = collector != nullptr ? collector : &resources.collectorProxy.wCollector;
        Collector& active = collector != nullptr ? static_cast<Collector&>(*collector)
                                                 : static_cast<Collector&>(resources.collectorProxy.wCollector);
        for (GCCycleGeneration generation : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
            auto& cycle = active.GetGenerationCycle(generation);
            if (cycle.Sequence() != 0) continue;
            if (!cycle.Snapshot().active) cycle.Begin(0);
            if (generation == GCCycleGeneration::YOUNG) {
                alignas(8) uint64_t storage[16] {};
                RememberedSet empty;
                empty.Initialize(reinterpret_cast<MAddress>(storage), sizeof(storage));
                GenerationSequenceFixture::AdvanceYoung(cycle, empty);
            } else {
                GenerationSequenceFixture::Advance(cycle);
            }
        }
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
        return collector.ResolveStoreValue(value, provenance, Generation::Old);
    }

    static void CheckStoreGoodTarget(WCollector& collector, BaseObject* value)
    {
        collector.CheckStoreGoodTarget("ForwardingLookupWitness", value,
            ForwardingProvenance{ ForwardingHolderKind::HeapRef, value, &value });
    }

    static BaseObject* ForwardUpdateRawRef(WCollector& collector, ObjectRef& root)
    {
        return collector.ForwardUpdateRawRef(root, Generation::Old);
    }

    static bool FixMinorField(WCollector& collector, RefField<>& field, BaseObject* knownBase = nullptr)
    {
        return collector.FixMinorEvacuatedSlot(field, knownBase, nullptr);
    }

    static bool FixMinorRoot(WCollector& collector, RootSlot& root)
    {
        return collector.FixMinorEvacuatedSlot(root, nullptr);
    }

    static BaseObject* TryForward(WCollector& collector, BaseObject* object)
    {
        return collector.TryForwardObject(object, Generation::Old);
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

    static FindToVersionResult ProductFindToVersion(WCollector& collector, BaseObject* from, Generation generation)
    {
        using ProductFn = FindToVersionResult (*)(const WCollector*, BaseObject*, Generation);
        void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        void* symbol = handle == nullptr ? nullptr : dlsym(
            handle, "_ZNK12MapleRuntime10WCollector13FindToVersionEPNS_10BaseObjectENS_10GenerationE");
        GC_EXPECT_TRUE(symbol != nullptr);
        Dl_info info {};
        GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                       std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
        if (info.dli_fname != nullptr) {
            std::fprintf(stderr, "FINDTO_PRODUCT_SO=%s\n", info.dli_fname);
        }
        FindToVersionResult result = symbol == nullptr
            ? FindToVersionResult::NotManaged()
            : reinterpret_cast<ProductFn>(symbol)(&collector, from, generation);
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
        WCollector& collector, BaseObject* from)
    {
        return collector.ForwardObjectExclusive(from);
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
        collector.PreforwardDiscoveredExternObjects(Generation::Old);
        collector.PreforwardAllResurrectExportFromObjects(Generation::Old);
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

    static void PublishColours(WCollector& collector) { ZGlobalsPointers::initialize(); }

    static uintptr_t DoubleBadColour(const WCollector& collector)
    {
        return ZPointerRemappedMask & ~ZPointerRemappedYoungMask &
            ~ZPointerRemappedOldMask;
    }

    static void RemapYoungRoots(WCollector& collector) { collector.RemapYoungRoots(); }

    static void FlipYoungRelocateStart(WCollector& collector)
    {
        ZGlobalsPointers::flip_young_relocate_start();
    }

    static void FlipOldRelocateStart(WCollector& collector)
    {
        ZGlobalsPointers::flip_old_relocate_start();
    }

    static RemsetConsumeResult ConsumeRemembered(WCollector& collector,
                                                  const std::unordered_set<MAddress>& previous,
                                                  BaseObject* currentMinorRoot)
    {
        ZGlobalsPointers::flip_young_mark_start();
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

uintptr_t OneLoadBadRemap()
{
    const uintptr_t bad = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    GC_EXPECT_TRUE(bad != 0);
    return bad & (~bad + 1);
}

class ResolveBarrier final : public Barrier {
public:
    ResolveBarrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet)
    {
    }

    BaseObject* Resolve(BaseObject* from) const
    {
        RefField<> field(StoreGoodPointer(from));
        auto& collector = static_cast<WCollector&>(theCollector);
        ZGlobalsPointers::flip_old_relocate_start();
        BaseObject* result = ReadReference(nullptr, field);
        ZGlobalsPointers::flip_old_relocate_start();
        return result;
    }
};

void EnsureDeliveryRuntime();

GcHeapFixture& ProductFixture()
{
    EnsureDeliveryRuntime();
    static GcHeapFixture fixture;
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
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

void EnsureDeliveryRuntime()
{
    static const int initialized = CJ_ScheduleManagerInit();
    GC_EXPECT_EQ(initialized, 0);
    LoadHealDeliveryRuntime::Ensure();
}

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

RegionInfo* ResetDeliveryUnit(GcHeapFixture& fx, size_t index)
{
    RegionInfo* previous = RegionInfo::GetRegionInfo(index);
    if (previous != nullptr) {
        RelocationReceiptTestAccess::ReleaseListOwnership(previous);
        if (previous->IsYoungRegion()) {
            previous->SetYoungRegionFlag(0);
        }
    }
    if (previous != nullptr && RegionInfo::TryGetRegionInfoAt(previous->GetRegionStart()) != nullptr) {
        RegionInfo::RetirePage(previous, []() {});
    }
    RegionInfo* region = RegionInfo::InitRegion(index, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->SetRegionAllocPtr(region->GetRegionStart());
    (void)fx;
    return region;
}

// ZObjectAllocator::PerAge::alloc_small_object uses a shared page, not a TLAB.
// Seed its ordinary allocation input for this synthetic heap. All CPU slots
// use the same page so migration cannot change the fixture's allocation input.
class DeliverySharedPageScope {
public:
    explicit DeliverySharedPageScope(RegionInfo* page)
        : manager(static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager())
    {
        // Heap::Init normally supplies this limit. The synthetic heap has one-unit pages.
        manager.SetLargeObjectThreshold(RegionInfo::UNIT_SIZE / KB);
        // zObjectAllocator.hpp:41 ZPerCPU<ZPage*>: every CPU slot names the page.
        auto& allocator = *manager.objectAllocators[untype(PageAge::old)];
        ZPerCPUIterator<RegionInfo*> slots(&allocator.sharedSmallPage);
        for (RegionInfo** slot; slots.next(&slot);) {
            previous.push_back(__atomic_exchange_n(slot, page, __ATOMIC_ACQ_REL));
        }
    }
    ~DeliverySharedPageScope()
    {
        auto& allocator = *manager.objectAllocators[untype(PageAge::old)];
        for (uint32_t cpu = 0; cpu < previous.size(); ++cpu) {
            allocator.sharedSmallPage.set(previous[cpu], cpu);
        }
    }
private:
    RegionManager& manager;
    std::vector<RegionInfo*> previous;
};

LiveInfo* PrepareForwardable(GcHeapFixture& fx, RegionInfo* region, MAddress liveObject)
{
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    if (region->IsAllocating()) GcHeapFixture::AdvanceGeneration(region->GetOwnerGeneration());
    for (MAddress address = region->GetRegionStart(); address < liveObject;) {
        BaseObject* prefix = fx.PlaceObject(address);
        address += prefix->GetSize();
        GC_EXPECT_TRUE(address <= liveObject);
    }
    LiveInfo* live = fx.PlantLiveInfo(region);
    const Generation generation = region->GetOwnerGeneration();
    RegionBitmap* bitmap = generation == Generation::Young
        ? fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize())
        : fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(liveObject);
    BaseObject* object = reinterpret_cast<BaseObject*>(liveObject);
    (void)bitmap->MarkBits(offset, object->GetSize(), region->GetRegionSize());
    // The product freezes the selected set before publishing any page view.
    if (ForwardingTable::GetEntries(region->GetRegionStart(), generation) == nullptr) {
        RegionList selected("publication-fixture");
        selected.PrependRegion(region, region->GetRegionType());
        GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(generation, selected));
        (void)selected.TakeHeadRegion();
    }
    if (generation == Generation::Young) {
        RelocationReceiptTestAccess::PrepareProductPage<Generation::Young>(region);
    } else {
        RelocationReceiptTestAccess::PrepareProductPage<Generation::Old>(region);
    }
    // This synthetic fixture leaves an unmaterialized allocation prefix.
    // Record the known object start explicitly; production freezes a dense
    // allocation walk inside PrepareForwardableRegion.
    return live;
}

void DestroyAfterGhostCleared(RegionInfo* region, const char* why)
{
    if (region != nullptr && region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    PublishGenerationMarkComplete(Generation::Young);
    PublishGenerationMarkComplete(Generation::Old);
    (void)why;
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
}

struct LateBackfillState {
    RegionInfo* region;
    RegionInfo* destination;
    BaseObject* from;
    BaseObject* to;
    LiveInfo* live;
    Generation generation;
};

LateBackfillState PrepareLateBackfill(GcHeapFixture& fx, WCollector& collector,
                                    Generation generation = Generation::Old, bool publishMapping = true)
{
    RegionInfo* region = ResetDeliveryUnit(fx, 5);
    RegionInfo* destination = ResetDeliveryUnit(fx, 2);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    if (generation == Generation::Young) region->SetYoungRegionFlag(1);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);

    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    BaseObject* to = fx.PlaceObject(destination->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));

    if (publishMapping) {
        const auto publication = ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
        collector.CopyObject(*from, *to, from->GetSize());
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, reinterpret_cast<MAddress>(from),
                                                   reinterpret_cast<MAddress>(to)), reinterpret_cast<MAddress>(to));
    }
    region->MarkForwardingDone();
    from->SetStateCode(ObjectState::FORWARDED);
    ZForwarding* table = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(from), generation);
    GC_EXPECT_TRUE(table != nullptr);
    return LateBackfillState{ region, destination, from, to, live,
                              region->GetOwnerGeneration() };
}

void CleanupLateBackfill(GcHeapFixture& fx, LateBackfillState& state)
{
    // The scenario has consumed its receipt. Normalize the planted header
    // before asking product retirement to prove no source still needs it.
    state.from->SetStateCode(ObjectState::NORMAL);
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    ForwardingTable::ResetRelocationSet(state.region->GetOwnerGeneration());
    state.region->metadata.liveInfo = nullptr;
    fx.FreePlanted(state.live);
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
    RegionInfo* region = ResetDeliveryUnit(fx, 1);
    RegionInfo* destination = ResetDeliveryUnit(fx, 2);
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
    ForwardingTable::ResetRelocationSet(state.region->GetOwnerGeneration());
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    state.region->metadata.liveInfo = nullptr;
    fx.FreePlanted(state.live);
}


LiveInfo* PlantOwnedLiveInfo(RegionInfo* region)
{
    LiveInfo* live = LiveInfoArena::GetLiveInfoArena().AllocateLiveInfo(region);
    live->bindedRegion = region;
    live->GetMarkFace().epoch.store(region->GetSnapshotEpoch(), std::memory_order_relaxed);
    region->metadata.liveInfo = live;
    return live;
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
    ForwardingTable::ResetRelocationSet(Generation::Young);
    ForwardingTable::ResetRelocationSet(Generation::Old);
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
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from), Generation::Old);
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
    GC_EXPECT_TRUE(afterCoverage.answer == ForwardingTable::ToAnswer::Unarmed);
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
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from), Generation::Old);
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
    GC_EXPECT_TRUE(afterCoverage.answer == ForwardingTable::ToAnswer::Unarmed);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, InsertionAndLateRekeyShareCurrentAuthority)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareValueRootForwarding(fx, collector);

    // Incoming registration receives a current value (ZGC load-good root).
    // The stored-root rekey below independently retains OLD-source coverage.
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    collector.ResurrectExportObject(state.to);
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
    collector.ResurrectExportObject(state.to);
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

// Keep the null boundary independently observable while the non-heap root
// adapter is pending P3 (advisor 606-20260915T021104Z).
GC_OTHER_VM_TEST(ValueRootCurrentization, NullControlRemainsStable)
{
    (void)ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    RelocationReceiptTestAccess::SeedValueRoots(collector, nullptr);
    const auto values = RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    const bool stable = AllVisitedEqual(values, nullptr);
    std::fprintf(stderr, "P1_NULL_CONTROL_ASSERT stable=%d\n", stable);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GC_EXPECT_TRUE(stable);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, NullAndNonHeapControlsRemainStable)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);

    RelocationReceiptTestAccess::SeedValueRoots(collector, nullptr);
    const std::vector<BaseObject*> nullValues =
        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    alignas(8) unsigned char nativeStorage[16] {};
    BaseObject* nonHeap = reinterpret_cast<BaseObject*>(nativeStorage);
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
    LoadHealDeliveryTestAccess::PublishColours(collector);
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
namespace {
void ExerciseMutatorCopy(bool runtimeEntry)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    RegionInfo* destination = ResetDeliveryUnit(fx, 3);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    const size_t size = from->GetSize();
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + size);
    const MAddress expected = destination->GetRegionStart();
    destination->SetRegionAllocPtr(expected);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    DeliverySharedPageScope allocation(destination);
    BaseObject* result = runtimeEntry
        ? RelocationReceiptTestAccess::ProductRelocateOrRemap(collector, from, region->generation_id())
        : RelocationReceiptTestAccess::ForwardImpl(collector, from, region);
    const MAddress mapping = ForwardingTable::FindTo(reinterpret_cast<MAddress>(from), Generation::Old);
    std::fprintf(stderr, "MUTATOR_COPY_ASSERT_EXECUTED runtime=%d result=%zx mapping=%zx expected=%zx\n",
                 runtimeEntry, reinterpret_cast<MAddress>(result), mapping, expected);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(result), expected);
    GC_EXPECT_EQ(mapping, expected);
    GC_EXPECT_TRUE(result->GetTypeInfo() == fx.typeInfo);
    GC_EXPECT_TRUE(from->IsForwarded());
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ResetRelocationSet(Generation::Old);
    region->DispelGhostFromRegion();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
}

GC_TEST(ForwardingPublicationProduct, MutatorRuntimeEntryReachesCopyAdmission)
{
    ExerciseMutatorCopy(true);
}

GC_TEST(ForwardingNoGeometry, ForwardImplTryLockCopiesWithoutPrebuiltMapping)
{
    // Historical name: ZGC admission now uses the page retain, not TryLock.
    ExerciseMutatorCopy(false);
}

GC_OTHER_VM_TEST(FindToPublicState, NotManagedIsObservable)
{
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, nullptr, Generation::Old);
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
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), static_cast<uint64_t>(0));
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, from, Generation::Old);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::NotForwarded);
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(1));
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), static_cast<uint64_t>(0));

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// A single product-linked construction exercises two distinct Unavailable producers.  It proves
// the route witness is not a constant formatter: one arm closes an installed publication while
// keeping its ghost region, and the other uses an unarmed, non-ghost region with a FORWARDED
// header. Both answers come from WCollector::FindToVersion in libcangjie-runtime.so.

// LookupTo returns the decision record itself.  Change both metadata faces only
// after the product lookup returns, then prove the record still describes the
// carrier inputs that selected Unavailable rather than those later faces.
GC_OTHER_VM_TEST(LookupDecisionSnapshot, SurvivesPostReturnGhostAndHeaderMutation)
{
    GcHeapFixture fixture;
    fixture.InstallPageOwner(fixture.region0);
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const Generation generation = fixture.region0->GetOwnerGeneration();
    const auto result = ForwardingTable::LookupTo(from, generation);
    const auto identity = reinterpret_cast<uintptr_t>(ForwardingTable::GetEntries(from, generation));
    const auto epoch = fixture.region0->GetSnapshotEpoch();
    const auto life = fixture.region0->GetRegionLifeId();
    ForwardingTable::ResetRelocationSet(generation);
    fixture.region0->BumpRegionLifeId();
    fixture.obj0->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_TRUE(result.answer == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(result.tableId, identity);
    GC_EXPECT_EQ(result.fromPageEpoch, epoch);
    GC_EXPECT_EQ(result.fromPageLifeId, life);
    GC_EXPECT_TRUE(result.forwardingSnapshotValid);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from, generation).answer == ForwardingTable::ToAnswer::Unarmed);
    fixture.obj0->SetStateCode(ObjectState::NORMAL);
}





// zRelocationSet.cpp:91-96 and zRelocate.cpp:1013-1047: retiring the old
// forwarding generation and installing the next one must not leave an object
// header claiming FORWARDED after the receipt that justified it is gone.

// Second family-8 path: a ROUTED page can retain a prior from->to receipt after
// raw-pin clears ghost and the next generation installs an empty active table.
// Retirement must preserve that receipt; active miss is not identity evidence.

// zGeneration.cpp:276-285 resets the old relocation set only after remap has
// consumed every source reference. A residual FORWARDED source proves that the
// port has not reached that state: keep its old receipt until a newer active
// generation publishes a successor instead of inferring identity at retirement.





GC_TEST(ForwardingPublicationProduct, ResolveStoreValueSafeAddrAfterForwardingTableGone)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(liveObject) + liveObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
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
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* liveObject = fx.PlaceObject(region->GetRegionStart());
    BaseObject* deadObject = fx.PlaceObject(region->GetRegionStart() + liveObject->GetSize());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(deadObject) + deadObject->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(liveObject));
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    ForwardingTable::RetainPageOwner(region)->set_in_place();
    const uint64_t sourceBirth = region->BirthSequence();
    const uint64_t sourceEpoch = region->GetSnapshotEpoch();
    manager.CompactRegion(region);
    const auto* source = ForwardingTable::GetFromPageView(region);
    const bool frozenSource = source != nullptr && source->birthSequence == sourceBirth && source->epoch == sourceEpoch;
    std::fprintf(stderr, "P1_INPLACE_BIRTH_ASSERT allocating=%d frozen_source=%d birth=%llu owner=%llu\n",
        region->IsAllocating(), frozenSource, static_cast<unsigned long long>(region->BirthSequence()),
        static_cast<unsigned long long>(region->GetSnapshotEpoch()));
    GC_EXPECT_TRUE(region->IsAllocating() && frozenSource);
    GC_EXPECT_TRUE(region->IsForwardingDone());

    const MAddress deadAddr = reinterpret_cast<MAddress>(deadObject);
    const MAddress liveAddr = reinterpret_cast<MAddress>(liveObject);
    GC_EXPECT_EQ(ForwardingTable::FindTo(deadAddr, Generation::Old), static_cast<MAddress>(0));
    GC_EXPECT_TRUE(ForwardingTable::FindTo(liveAddr, Generation::Old) != static_cast<MAddress>(0));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart(), Generation::Old) != nullptr);
    GC_EXPECT_TRUE(region->GetRegionType() != RegionInfo::RegionType::THREAD_LOCAL_REGION);
    GC_EXPECT_TRUE(region->GetRegionType() == RegionInfo::RegionType::RECENT_FULL_REGION);

    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
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



// The receipt, retirement and lookup all belong to the linked product SO.
// Save the expected identity from the actual publisher before retiring it;
// no LookupResult is constructed or passed to a product consumer by this test.
struct LookupWitnessIdentity {
    uintptr_t tableId;
    MAddress start;
    uint64_t epoch;
    RegionLifeId lifeId;
};

LookupWitnessIdentity ReadLookupWitnessIdentity(ZForwarding* table)
{
    GC_EXPECT_TRUE(table != nullptr);
    const ZForwarding::FromPageView* view = table->from_page_snapshot();
    GC_EXPECT_TRUE(view != nullptr);
    return { reinterpret_cast<uintptr_t>(table), table->start(),
             view->epoch, view->lifeId };
}

void ExpectDiagnosticLookupIdentity(const std::string& output, const LookupWitnessIdentity& expected)
{
    char table[64] {};
    (void)std::snprintf(table, sizeof(table), "table_id=%#zx ", static_cast<size_t>(expected.tableId));
    const std::string epoch = "from_page_epoch=" + std::to_string(expected.epoch) + " ";
    const std::string life = "lifeId=" + std::to_string(expected.lifeId) + " ";
    const bool tableIdentityMatches = output.find(table) != std::string::npos;
    const bool fromPageEpochMatches = output.find(epoch) != std::string::npos;
    const bool fromPageLifeIdMatches = output.find(life) != std::string::npos;
    // Print every comparison before a throwing assertion: a field-specific
    // product cut must change only its corresponding result in this record.
    std::fprintf(stderr, "LOOKUP_WITNESS_TARGET diagnostic table=%d epoch=%d life=%d\n",
                 tableIdentityMatches, fromPageEpochMatches, fromPageLifeIdMatches);
    GC_EXPECT_TRUE(tableIdentityMatches);
    GC_EXPECT_TRUE(fromPageEpochMatches);
    GC_EXPECT_TRUE(fromPageLifeIdMatches);
}

void CheckLookupWitness(bool publishReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector, Generation::Old, false);
    const MAddress from = reinterpret_cast<MAddress>(state.from);
    const MAddress to = reinterpret_cast<MAddress>(state.to);
    const auto expected = ReadLookupWitnessIdentity(ForwardingTable::GetEntries(from, state.generation));
    if (publishReceipt) {
        auto publication = ForwardingTable::EnsurePublicationBeforeCopy(state.region, from);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, to), to);
    }
    const auto lookup = ForwardingTable::LookupTo(from, state.generation);
    GC_EXPECT_EQ(lookup.to, publishReceipt ? to : 0);
    GC_EXPECT_TRUE(lookup.answer == (publishReceipt ? ForwardingTable::ToAnswer::ArmedHit :
                                                    ForwardingTable::ToAnswer::ArmedMiss));
    GC_EXPECT_EQ(lookup.tableId, expected.tableId);
    GC_EXPECT_EQ(lookup.carrierStart, expected.start);
    GC_EXPECT_EQ(lookup.fromPageEpoch, expected.epoch);
    GC_EXPECT_EQ(lookup.fromPageLifeId, expected.lifeId);
    GC_EXPECT_TRUE(lookup.forwardingSnapshotValid);
    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, ActiveHitIdentifiesPublisher)
{
    CheckLookupWitness(true);
}

GC_OTHER_VM_TEST(ForwardingLookupWitness, ActiveMissKeepsCandidateIdentity)
{
    CheckLookupWitness(false);
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



GC_OTHER_VM_TEST(NeverInstalledDiagnostic, NeverInstalledListsAllCoveringCarriers)
{
    GcHeapFixture fixture;
    RegionList selected("diagnostic-generations");
    selected.PrependRegion(fixture.region0, fixture.region0->GetRegionType());
    fixture.region0->SetYoungRegionFlag(1);
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Young, selected));
    fixture.region0->SetYoungRegionFlag(0);
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Old, selected));
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const auto snapshot = ForwardingTable::CaptureNeverInstalledSnapshot(from);
    GC_EXPECT_EQ(snapshot.carrierTotal, 2u);
    GC_EXPECT_EQ(snapshot.carrierCount, 2u);
    GC_EXPECT_FALSE(snapshot.carrierOverflow);
    for (Generation generation : {Generation::Young, Generation::Old}) {
        const auto identity = reinterpret_cast<uintptr_t>(ForwardingTable::GetEntries(from, generation));
        bool matched = false;
        for (size_t i = 0; i < snapshot.carrierCount; ++i) {
            const auto& carrier = snapshot.carriers[i];
            if (carrier.tableId == identity) {
                GC_EXPECT_EQ(carrier.tableGeneration, static_cast<uint8_t>(generation));
                GC_EXPECT_EQ(carrier.start, fixture.region0->GetRegionStart());
                matched = true;
            }
        }
        GC_EXPECT_TRUE(matched);
    }
    ForwardingTable::ResetRelocationSet(Generation::Young);
    ForwardingTable::ResetRelocationSet(Generation::Old);
    (void)selected.TakeHeadRegion();
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










// zStackWatermark.cpp:159-186: process the head before phase completion.
GC_TEST(ForwardingPublicationProduct, WatermarkRemapsInvisibleAndNativeHeadBeforeCompletion)
{
    auto& fixture = ProductFixture();
    Mutator mutator;
    mutator.SetManagedContext(false);
    mutator.PublishInvisibleRoot(fixture.obj0);
    ObjectRef* native = mutator.AddNativeFrameRoot(fixture.obj0);
    size_t nativeVisits = 0;
    size_t invisibleVisits = 0;
    using P = StackWatermark::ProcessingPhase;
    RootVisitor ordinary = [&](RootSlot& slot) {
        if (!is_null(slot.LoadPlain())) {
            ++nativeVisits;
            StorePlain(slot, from_object(fixture.obj1));
        }
    };
    RootVisitor invisible = [&](RootSlot& slot) {
        GC_EXPECT_FALSE(mutator.GetStackWatermark().IsDone(17, P::REMAP));
        GC_EXPECT_EQ(raw(slot.LoadPlain()), reinterpret_cast<uintptr_t>(fixture.obj0));
        StorePlain(slot, from_object(fixture.obj1));
        ++invisibleVisits;
    };
    auto derived = Mutator::MakeDerivedRootVisitor(ordinary);
    size_t frames = 0;
    bool complete = mutator.DrainStackWatermark(ordinary, invisible, 17, StackWatermark::WM_OWNER_SELF,
                                               &derived, frames, false, P::REMAP);
    GC_EXPECT_TRUE(complete);
    GC_EXPECT_TRUE(mutator.GetStackWatermark().IsDone(17, P::REMAP));
    GC_EXPECT_EQ(nativeVisits, size_t(1));
    GC_EXPECT_EQ(invisibleVisits, size_t(1));
    GC_EXPECT_TRUE(mutator.WithdrawInvisibleRoot() == fixture.obj1);
    GC_EXPECT_EQ(raw(native->LoadPlain()), reinterpret_cast<uintptr_t>(fixture.obj1));
    mutator.RemoveNativeFrameRoot(native);
}

// ProcessDerivedOop (oopMap.cpp:400-421): shared base, two distinct offsets.
GC_TEST(ForwardingPublicationProduct, DerivedClosurePreservesSharedBaseOffsets)
{
    RootSlot base;
    StorePlain(base, to_zaddress(0x10000));
    DerivedSlot first;
    DerivedSlot second;
    RebaseDerived(first, base, 8);
    RebaseDerived(second, base, 24);
    size_t visits = 0;
    RootVisitor root = [&](RootSlot& slot) {
        GC_EXPECT_EQ(raw(slot.LoadPlain()), uintptr_t(0x10000));
        StorePlain(slot, to_zaddress(0x20000));
        ++visits;
    };
    auto derived = Mutator::MakeDerivedRootVisitor(root);
    derived(base.LoadPlain(), first);
    derived(base.LoadPlain(), second);
    GC_EXPECT_EQ(raw(base.LoadPlain()), uintptr_t(0x10000));
    root(base);
    GC_EXPECT_EQ(raw(first.LoadDerived()), uintptr_t(0x20008));
    GC_EXPECT_EQ(raw(second.LoadDerived()), uintptr_t(0x20018));
    GC_EXPECT_EQ(visits, size_t(3));
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

void InitializeDerivedBaseMap(bool tagged)
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

}

void RunDerivedBaseProducer(bool tagged, bool moving = false, bool expectFailClosed = false,
                            bool unresolvedGhost = false)
{
    InitializeDerivedBaseMap(tagged);
    auto& image = derivedBaseMapImage;

    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LateBackfillState state {};
    if (unresolvedGhost) {
        state = PrepareLateBackfill(fx, collector);
        state.from->SetStateCode(ObjectState::NORMAL);
        state.region->MarkForwardingDone();
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
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
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
    const bool usesState = moving || unresolvedGhost;
    const uintptr_t base = reinterpret_cast<uintptr_t>(usesState ? state.from : fx.obj0);
    const uintptr_t expected = reinterpret_cast<uintptr_t>(moving ? state.to : fx.obj0);
    uintptr_t frame[8] = {};
    frame[0] = base;
    frame[1] = base + 8;
    frame[2] = reinterpret_cast<uintptr_t>(image.pc) + 9;
    Mutator mutator;
    auto& context = mutator.GetUnwindContext();
    context.frameInfo.mFrame.SetIP(image.pc);
    context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frame[3]));
    context.anchorFA = nullptr;
    std::fprintf(stderr, "DERIVED_BASE_INPUT tagged=%d moving=%d base=%zx derived=%zx\n",
                 tagged, moving, frame[0], frame[1]);
    if (expectFailClosed) {
        AbortCapture aborted = CaptureAbort([&]() {
            mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_PREFORWARD, false);
        });
        std::fprintf(stderr, "DERIVED_BASE_FAILCLOSED status=%d\n%s", aborted.status, aborted.output.c_str());
        if (usesState) { CleanupLateBackfill(fx, state); }
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
        std::fprintf(stderr, "DERIVED_BASE_TARGET target_assertion executed=1 matched=%d\n",
                     aborted.output.find("should be forwarded from=") !=
                         std::string::npos);
        GC_EXPECT_TRUE(aborted.output.find("should be forwarded from=") !=
                       std::string::npos);
        GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
        GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
        return;
    }
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_PREFORWARD, false);
    std::fprintf(stderr, "DERIVED_BASE_RESULT base=%zx derived=%zx expected=%zx\n", frame[0], frame[1], expected + 8);
    const bool baseCorrect = frame[0] == expected;
    const bool derivedCorrect = frame[1] == expected + 8;
    if (usesState) { CleanupLateBackfill(fx, state); }
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GC_EXPECT_TRUE(derivedCorrect);
    GC_EXPECT_TRUE(baseCorrect);
}
}

GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedBaseProducer)
{
    RunDerivedBaseProducer(true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedOrdinaryBaseProducer)
{
    RunDerivedBaseProducer(false);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedMovingBaseProducer)
{
    RunDerivedBaseProducer(true, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedOrdinaryMovingBaseProducer)
{
    RunDerivedBaseProducer(false, true);
}
GC_OTHER_VM_TEST(ForwardingPublicationProduct, PreForwardDerivedTaggedUnresolvedGhostFailsClosed)
{
    RunDerivedBaseProducer(true, false, true, true);
}
#endif

GC_TEST(ForwardingPublicationProduct, PreForwardDerivedRebasesFromRemappedBaseWithoutLookup)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ResetRelocationSet(state.region->GetOwnerGeneration());
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(
        state.region->GetRegionStart(), state.generation) == nullptr);

    constexpr size_t derivedOffset = sizeof(uintptr_t);
    RootSlot oldBase;
    StorePlain(oldBase, from_object(state.from));
    DerivedSlot derived;
    RebaseDerived(derived, oldBase, derivedOffset);

    const uint64_t hitsBefore = ForwardingTable::ArmedHitCount();
    const uint64_t missesBefore = ForwardingTable::ArmedMissCount();
    const uint64_t unavailableBefore = ForwardingTable::UnarmedCount();
    const uint64_t unarmedBefore = ForwardingTable::UnarmedCount();
    size_t resolverCalls = 0;
    DerivedPtrVisitor visitor = Mutator::MakeDerivedRootVisitor(
        [&](RootSlot& slot) {
            BaseObject* old = to_object(safe(slot.LoadPlain()));
            ++resolverCalls;
            GC_EXPECT_TRUE(old == state.from);
            StorePlain(slot, from_object(state.to));
        });
    visitor(oldBase.LoadPlain(), derived);

    GC_EXPECT_EQ(resolverCalls, static_cast<size_t>(1));
    GC_EXPECT_EQ(raw(derived.LoadDerived()),
                 reinterpret_cast<MAddress>(state.to) + derivedOffset);
    GC_EXPECT_EQ(ForwardingTable::ArmedHitCount(), hitsBefore);
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), missesBefore);
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), unavailableBefore);
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), unarmedBefore);

    // Positive control for the zero-lookup assertion above: the same closed carrier and old base
    // must move the unavailable counter when the forwarding lookup is explicitly invoked.
    const auto lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(state.from), Generation::Old);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::Unarmed);
    GC_EXPECT_EQ(lookup.to, MAddress(0));
    GC_EXPECT_EQ(ForwardingTable::UnarmedCount(), unavailableBefore + 1);

    CleanupLateBackfill(fx, state);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// A non-LookupUnavailable route may carry lookup-shaped fields from a caller,
// but with the snapshot validity bit cleared they must never be rendered as
// legal-looking zero values.




// SD forwarding consumer gate: a compacted destination can be classified as
// kAlreadyToStart by reverse geometry, but that classification is not a
// load-good receipt. Make the destination header FORWARDED and drive the
// product ResolveStoreValue entry; the only legal result is fail-closed.










// zRelocate.cpp:382-415 and zForwarding.inline.hpp:267-303: an inserted
// winner, including an in-place identity, is the result of subsequent lookups.
// The removed WaitRouted observation branches have no product counterpart;
// exercise their surviving address invariant without lookup-count scheduling.
static void CheckForwardingWinner(bool identity)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    const MAddress fromAddr = reinterpret_cast<MAddress>(from);
    region->SetRegionAllocPtr(fromAddr + from->GetSize());
    BaseObject* winner = identity ? from : fx.obj1;
    BaseObject* loser = identity ? fx.obj1 : from;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, fromAddr);
    GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddr, Generation::Old), 0);
    GC_EXPECT_FALSE(region->IsForwardingDone());
    {
        auto publication = ForwardingTable::EnsurePublicationBeforeCopy(region, fromAddr);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, fromAddr,
                         reinterpret_cast<MAddress>(winner)), reinterpret_cast<MAddress>(winner));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, fromAddr,
                         reinterpret_cast<MAddress>(loser)), reinterpret_cast<MAddress>(winner));
    }
    {
        RegionInfo::RetainScope lease(region);
        GC_EXPECT_TRUE(lease.ok());
        GC_EXPECT_TRUE(RelocationReceiptTestAccess::WaitRoutedTipReady(
                           collector, from, nullptr, region) == winner);
        GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddr, Generation::Old), reinterpret_cast<MAddress>(winner));
    }
    region->MarkForwardingDone();
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddr, Generation::Old), reinterpret_cast<MAddress>(winner));
    region->ReleaseForwarding();
    GC_EXPECT_FALSE(region->RetainForwarding());
    GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddr, Generation::Old), reinterpret_cast<MAddress>(winner));

    region->DispelGhostFromRegion();
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

GC_TEST(ForwardingPublicationProduct, ForwardingIdentityWinnerSurvivesDoneAndRelease)
{
    CheckForwardingWinner(true);
}

GC_TEST(ForwardingPublicationProduct, ForwardingMovedWinnerSurvivesDoneAndRelease)
{
    CheckForwardingWinner(false);
}

// zRelocate.cpp:412-415: completing a page is not a forwarding receipt.
// Preserve the old published-miss rejection invariant at the current product
// exit, without requiring the deleted WaitRouted diagnostic branch or fields.
GC_TEST(ForwardingPublicationProduct, CompletedForwardingMissRejectsOriginalAddress)
{
    GcHeapFixture& fx = ProductFixture();
    AbortCapture captured = CaptureAbort([&]() {
        RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
        RegionInfo* region = ResetDeliveryUnit(fx, 4);
        GC_EXPECT_TRUE(region != nullptr);
        region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        BaseObject* from = fx.PlaceObject(region->GetRegionStart());
        const MAddress fromAddr = reinterpret_cast<MAddress>(from);
        region->SetRegionAllocPtr(fromAddr + from->GetSize());
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
        (void)PrepareForwardable(fx, region, fromAddr);
        region->MarkForwardingDone();
        region->ReleaseForwarding();
        GC_EXPECT_TRUE(region->IsForwardingDone());
        GC_EXPECT_FALSE(region->RetainForwarding());
        GC_EXPECT_EQ(ForwardingTable::FindTo(fromAddr, Generation::Old), 0);
        std::fprintf(stderr, "COMPLETED_FORWARDING_MISS_ENTRY from=%p\n", from);
        (void)RelocationReceiptTestAccess::ProductRelocateOrRemap(
            collector, from, region->generation_id());
    });
    GC_EXPECT_TRUE(WIFSIGNALED(captured.status));
    GC_EXPECT_EQ(WTERMSIG(captured.status), SIGABRT);
    GC_EXPECT_TRUE(captured.output.find("COMPLETED_FORWARDING_MISS_ENTRY") != std::string::npos);
    GC_EXPECT_TRUE(captured.output.find(
        "ZRelocate::forward_object requires a forwarding entry") != std::string::npos);
}

GC_TEST(ForwardingPublicationProduct, CompactedWithoutFwdDoneWaitsInProductSO)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
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
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
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
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    ObjectRef root;
    StorePlain(root, from_object(from));
    BaseObject* resolved = RelocationReceiptTestAccess::ForwardUpdateRawRef(collector, root);
    GC_EXPECT_TRUE(resolved == to);
    GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<MAddress>(to));

    publication = ForwardingTable::Publication();
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
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
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    GC_EXPECT_TRUE(region != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* from = fx.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(from));
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    ExpectRootAbortAt("ForwardUpdateRawRef.unresolved", [&]() {
        ObjectRef root;
        StorePlain(root, from_object(from));
        (void)RelocationReceiptTestAccess::ForwardUpdateRawRef(collector, root);
    });
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
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
    RegionInfo* firstRegion = ResetDeliveryUnit(fx, 5);
    RegionInfo* secondRegion = ResetDeliveryUnit(fx, 4);
    RegionInfo* finalRegion = ResetDeliveryUnit(fx, 3);
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
    RegionList selected("forwarding-chain-fixture");
    selected.PrependRegion(firstRegion, firstRegion->GetRegionType());
    selected.PrependRegion(secondRegion, secondRegion->GetRegionType());
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Old, selected));
    while (selected.TakeHeadRegion() != nullptr) {}
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
    ForwardingTable::ResetRelocationSet(firstRegion->GetOwnerGeneration());
    ForwardingTable::ResetRelocationSet(secondRegion->GetOwnerGeneration());
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

    {
        DeliverySharedPageScope allocation(state.destination);
        ZForwardingLife::PageWorkScope task(ForwardingTable::RetainPageOwner(state.region).get(), true);
        GC_EXPECT_TRUE(manager.RelocateClaimedPage(state.region));
    }

    (void)queue.Wait(request.request);
    const MAddress receipt = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(receipt, expected);
    GC_EXPECT_TRUE(receipt != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), expected);
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

    ForwardingTable::RetainPageOwner(state.region)->set_in_place();
    manager.CompactRegion(state.region);
    state.region->MarkForwardingDone();

    (void)queue.Wait(request.request);
    const MAddress receipt = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(receipt, expected);
    GC_EXPECT_TRUE(receipt != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), expected);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(expected)->IsValidObject());
    RefField<> qualified = RelocationReceiptTestAccess::QualifyStoreValue(
        collector, reinterpret_cast<BaseObject*>(expected));
    GC_EXPECT_EQ(raw(qualified.GetTargetObject()), expected);
    RefField<> productField(qualified);
    (void)RelocationReceiptTestAccess::FixMinorField(collector, productField);
    GC_EXPECT_EQ(raw(productField.GetTargetObject()), expected);
    RefField<> derivedField(ZAddress::store_good(to_zaddress(expected + 8u)));
    (void)RelocationReceiptTestAccess::FixMinorField(
        collector, derivedField, reinterpret_cast<BaseObject*>(expected));
    GC_EXPECT_EQ(raw(derivedField.GetTargetObject()), expected + 8u);
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
}

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
GC_TEST(ForwardingPublicationProduct, PageWaitThenLookupReadsOriginalCompactReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    RegionInfo* routeDestination =
        ResetDeliveryUnit(fx, 3);
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
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    buffer->SetRegion(routeDestination);
    // Page work starts below; RouteRegion now waits for that work to finish.
    // The precondition is an installed, unfinished forwarding table.
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from, Generation::Old));
    GC_EXPECT_FALSE(region->IsForwardingDone());
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
    BaseObject* workerResult = reinterpret_cast<BaseObject*>(ForwardingTable::FindTo(from, Generation::Old));
    const bool workerClosed = queue.PendingCount() == 0;
    waiter.join();
    buffer->ClearRegion();

    GC_EXPECT_TRUE(resolved != nullptr);
    GC_EXPECT_TRUE(resolved != liveObject);
    GC_EXPECT_TRUE(seeded.accepted);
    GC_EXPECT_TRUE(claimed != nullptr);
    GC_EXPECT_TRUE(resolved == workerResult);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), reinterpret_cast<MAddress>(resolved));
    GC_EXPECT_TRUE(workerClosed);

    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
#endif // MRT_PRODUCT_TESTABLE_INTERNALS

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
GC_TEST(ForwardingPublicationProduct, CompletedPageResolvesThroughForwardingTable)
{
    GcHeapFixture& fx = ProductFixture();
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(4));
    RelocationReceiptTestAccess::ReleaseListOwnership(RegionInfo::GetRegionInfo(3));
    RegionInfo* region = ResetDeliveryUnit(fx, 4);
    RegionInfo* routeDestination =
        ResetDeliveryUnit(fx, 3);
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
    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    routeDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    buffer->SetRegion(routeDestination);
    DeliverySharedPageScope allocation(routeDestination);
    GC_EXPECT_TRUE(manager.RelocateClaimedPage(region));
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
    BaseObject* workerResult = reinterpret_cast<BaseObject*>(ForwardingTable::FindTo(from, Generation::Old));
    const bool workerClosed = queue.PendingCount() == 0;
    waiter.join();
    buffer->ClearRegion();

    const bool resolvedExpected = resolved != nullptr;
    const bool resolvedMoved = resolved != fromObject;
    const bool requestAccepted = seeded.accepted;
    const bool requestClaimed = claimed != nullptr;
    const bool workerMatched = resolved == workerResult;
    const bool tablePublished = ForwardingTable::FindTo(from, Generation::Old) == reinterpret_cast<MAddress>(resolved);
    const bool generationClosed = workerClosed;

    collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
#endif // MRT_PRODUCT_TESTABLE_INTERNALS

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

    ForwardingTable::RetainPageOwner(region)->set_in_place();
    manager.CompactRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());

    (void)queue.Wait(request.request);
    const MAddress resolved = request.request->page_forwarding()->find(from);
    GC_EXPECT_EQ(resolved, start);
    GC_EXPECT_TRUE(resolved != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), resolved);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(resolved)->IsValidObject());

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    RelocationReceiptTestAccess::ReleaseListOwnership(region);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(ForwardingPublicationProduct, PostRemapResetDestroysInstalledSet)
{
    GcHeapFixture fixture;
    fixture.region0->SetYoungRegionFlag(1);
    fixture.InstallPageOwner(fixture.region0);
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fixture.obj1);
    {
        auto publication = ForwardingTable::EnsurePublicationBeforeCopy(fixture.region0, from);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, to), to);
    }
    auto owner = ForwardingTable::RetainPageOwner(fixture.region0);
    GC_EXPECT_TRUE(static_cast<bool>(owner));
    owner->release_page();
    owner->mark_done();
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Young), to);
    ForwardingTable::ResetRelocationSet(Generation::Young);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from, Generation::Young) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from, Generation::Young).answer == ForwardingTable::ToAnswer::Unarmed);
}



// ClearEntries must seal an installed table and wait for the publication owner
// that crossed the copy boundary.  The owner inserts while clear is waiting;
// only after the owner releases may clear unlink and retire the table.

// zRelocationSet.cpp:191-197: clearing the table waits for outstanding table
// users independently of the source-page count. Observe drain admission and
// the held publication token before allowing the publisher to complete.

// zRelocate.cpp:362-372: Exclusive owns the before-copy Publication through
// CopyObject, receipt installation, queue publication and FORWARDED state.  Use
// the product allocator's real queue so no receipt is hand-fed by this test.
GC_TEST(ForwardingPublicationProduct, ExclusiveCopyPublishesProductReceipt)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = ResetDeliveryUnit(fx, 0);
    RegionInfo* destination = ResetDeliveryUnit(fx, 1);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    BaseObject* fromObject = fx.PlaceObject(region->GetRegionStart() + 64);
    BaseObject* toObject = fx.PlaceObject(destination->GetRegionStart() + 64);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(fromObject) + fromObject->GetSize());
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(toObject));
    const MAddress from = reinterpret_cast<MAddress>(fromObject);
    const MAddress to = reinterpret_cast<MAddress>(toObject);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    LiveInfo* live = PrepareForwardable(fx, region, from);

    DeliverySharedPageScope allocation(destination);
    BaseObject* relocated =
        RelocationReceiptTestAccess::ForwardExclusive(collector, fromObject);

    const bool productPublished = ForwardingTable::FindTo(from, Generation::Old) != 0;
    GC_EXPECT_TRUE(productPublished);
    GC_EXPECT_TRUE(relocated == toObject);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), to);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Old), to);
    GC_EXPECT_TRUE(fromObject->IsForwarded());

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
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
    std::fprintf(stderr, "SOURCE_LIVEMAP_ASSERT_EXECUTED live=%d dead=%d\n",
                 region->IsOwnerSurvivedObject(offset), region->IsOwnerSurvivedObject(0));
    GC_EXPECT_TRUE(region->IsOwnerSurvivedObject(offset));
    GC_EXPECT_FALSE(region->IsOwnerSurvivedObject(0));

    region->DispelGhostFromRegion();
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
    // Source-page release does not end forwarding lifetime (zRelocationSet.cpp:197).
    GC_EXPECT_TRUE(ForwardingTable::GetFromPageView(region) != nullptr);
    ForwardingTable::ResetRelocationSet(region->GetOwnerGeneration());
    GC_EXPECT_TRUE(ForwardingTable::GetFromPageView(region) == nullptr);
    GC_EXPECT_FALSE(region->IsOwnerSurvivedObject(offset));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    fx.FreePlanted(live);
}

// ZGC zRelocate.cpp:1256-1279: the promoted page keeps the relocation-set
// livemap selected at registration, and discharge walks only that live set.
GC_TEST(LoadHealDeliveryProduct, FlipPromotedPageRemembersOnlyLiveHolder)
{
    GcHeapFixture& fx = ProductFixture();
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

    LiveInfo* live = PlantOwnedLiveInfo(holderRegion);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, holderRegion->GetRegionSize());
    (void)bitmap->MarkBits(0, objectSize, holderRegion->GetRegionSize());
    RegionManager manager;
    manager.AddFlipPromotedPage(holderRegion);
    holderRegion->metadata.liveInfo = nullptr;
    holderRegion->SetYoungRegionFlag(0);
    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    GCWorkers workers(GCWorkers::Generation::YOUNG, 2);
    workers.SetActive();
    manager.RememberFlipPromotedPages(workers);
    workers.SetInactive();
    GC_EXPECT_TRUE(remembered.Contains(reinterpret_cast<MAddress>(liveField)));
    GC_EXPECT_FALSE(remembered.Contains(reinterpret_cast<MAddress>(deadField)));
    std::unordered_set<MAddress> previous;
    remembered.FlipForMinor();
    remembered.ScanPreviousForMinor(previous);
    GC_EXPECT_EQ(previous.count(reinterpret_cast<MAddress>(liveField)), 1u);
    GC_EXPECT_EQ(previous.count(reinterpret_cast<MAddress>(deadField)), 0u);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    EmptyBothRememberedFaces(remembered);
    // manager owns the original map through its promotion page.
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
    GcHeapFixture::AdvanceGeneration(Generation::Young);
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
        fromField->StoreColoured(ColouredPointer(youngTarget, OneLoadBadRemap()));
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

// ZRelocateWork::update_remset_promoted: young targets are remembered;
// old targets are remapped without adding a remembered bit.
GC_TEST(LoadHealDeliveryProduct, PromotedObjectRemembersYoungTargetOnly)
{
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* targetRegion = ResetDeliveryUnit(fx, 1);
    targetRegion->SetYoungRegionFlag(1);
    BaseObject* holder = fx.PlaceObject(holderRegion->GetRegionStart());
    BaseObject* target = fx.PlaceObject(targetRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(holder) + holder->GetSize());
    targetRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(target) + target->GetSize());
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    field.StoreColoured(GcUnit::StoreGoodPointer(target));
    RegionManager::RememberPromotedObject(holder);
    GC_EXPECT_TRUE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
    EmptyBothRememberedFaces(remembered);
    targetRegion->SetYoungRegionFlag(0);
    RegionManager::RememberPromotedObject(holder);
    GC_EXPECT_FALSE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
}

// zRelocate.cpp:780-784,1241: resolve a young from-address to an old
// target and heal the field before omitting its remset entry.
GC_TEST(LoadHealDeliveryProduct, PromotedFieldsHealForwardedOldTarget)
{
    GcHeapFixture& fx = ProductFixture();
    for (bool flipPromoted : {false, true}) {
        RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
        RegionInfo* fromRegion = ResetDeliveryUnit(fx, 4);
        RegionInfo* toRegion = ResetDeliveryUnit(fx, 2);
        fromRegion->SetYoungRegionFlag(1);
        if (flipPromoted) holderRegion->SetYoungRegionFlag(1);
        BaseObject* holder = fx.PlaceObject(holderRegion->GetRegionStart());
        BaseObject* from = fx.PlaceObject(fromRegion->GetRegionStart());
        BaseObject* to = fx.PlaceObject(toRegion->GetRegionStart());
        holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(holder) + holder->GetSize());
        fromRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
        toRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
        auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
        RememberedSet& remembered = DeliveryRememberedSet(fx);
        EmptyBothRememberedFaces(remembered);
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
        LoadHealDeliveryTestAccess::PublishColours(collector);
        LiveInfo* fromLive = PrepareForwardable(fx, fromRegion, reinterpret_cast<MAddress>(from));
        field.StoreColoured(GcUnit::StoreGoodPointer(from));
        LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
        const zpointer before = field.GetFieldValue();
        GC_EXPECT_FALSE(ZPointer::is_load_good((field).GetFieldValue()));
        GC_EXPECT_TRUE(ForwardingTable::GetCovering(reinterpret_cast<MAddress>(from), Generation::Young) != nullptr);
        if (!flipPromoted) {
            // Unfinished relocation must remain deferred, without waiting.
            RegionManager::RememberPromotedObject(holder);
            GC_EXPECT_EQ(raw(field.GetFieldValue()), raw(before));
            GC_EXPECT_TRUE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
            EmptyBothRememberedFaces(remembered);
        }
        ForwardingTable::Publication publication = ForwardingTable::EnsurePublicationBeforeCopy(
            fromRegion, reinterpret_cast<MAddress>(from));
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, reinterpret_cast<MAddress>(from),
                                                   reinterpret_cast<MAddress>(to)),
                     reinterpret_cast<MAddress>(to));
        LiveInfo* holderLive = nullptr;
        if (flipPromoted) {
            holderLive = PlantOwnedLiveInfo(holderRegion);
            RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(holderLive, holderRegion->GetRegionSize());
            (void)bitmap->MarkBits(0, holder->GetSize(), holderRegion->GetRegionSize());
            RegionManager manager;
            manager.AddFlipPromotedPage(holderRegion);
            GCWorkers workers(GCWorkers::Generation::YOUNG, 2);
            workers.SetActive();
            manager.RememberFlipPromotedPages(workers);
            workers.SetInactive();
        } else {
            RegionManager::RememberPromotedObject(holder);
        }
        GC_EXPECT_TRUE(to_object(field.GetTargetObject()) == to);
        GC_EXPECT_TRUE(ZPointer::is_load_good((field).GetFieldValue()));
        GC_EXPECT_FALSE(remembered.Contains(reinterpret_cast<MAddress>(&field)));
        const uintptr_t markBits = ZPointerMarkedYoungMask | ZPointerMarkedOldMask;
        GC_EXPECT_EQ(raw(field.GetFieldValue()) & markBits, raw(before) & markBits);
        if (holderLive != nullptr) {
            holderRegion->metadata.liveInfo = nullptr;
            // The promotion page already destroyed its owned source map.
        }
        ForwardingTable::ResetRelocationSet(fromRegion->GetOwnerGeneration());
        fromRegion->metadata.liveInfo = nullptr;
        fx.FreePlanted(fromLive);
        fromRegion->SetYoungRegionFlag(0);
        LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    }
}

// Direct semantic matrix for the current remembered face. The reference array
// is live, and its far field lies beyond the former 64-byte
// recovery window. ZGC still applies the load barrier because the current old
// page, rather than an object-level recovery guess, is the admission unit.
GC_TEST(LoadHealDeliveryProduct, CurrentRemsetRemapsLiveRemoteArrayField)
{
    ZStat::Initialize();
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
    GcHeapFixture::AdvanceGeneration(Generation::Old);
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
        nearField->StoreColoured(ColouredPointer(youngTarget, OneLoadBadRemap()));
        farField->StoreColoured(ColouredPointer(youngTarget, OneLoadBadRemap()));
        youngField->StoreColoured(ColouredPointer(youngTarget, OneLoadBadRemap()));
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
    GC_EXPECT_EQ(raw(farField->GetFieldValue()) & ZPointerRemappedMask, doubleBad);
    const uintptr_t youngBefore = raw(youngField->GetFieldValue());
    LoadHealDeliveryTestAccess::RemapYoungRoots(collector);

    const bool nearResolved = to_object(nearField->GetTargetObject()) == forwarding.to;
    const bool farResolved = to_object(farField->GetTargetObject()) == forwarding.to;
    const bool nearStoreGood = ZPointer::is_store_good((*nearField).GetFieldValue());
    const bool farStoreGood = ZPointer::is_store_good((*farField).GetFieldValue());
    const bool youngUnchanged = raw(youngField->GetFieldValue()) == youngBefore;
    const bool holderNonAllocating = !holderRegion->IsAllocating();
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
    ZStat::Initialize();
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
    GcHeapFixture::AdvanceGeneration(Generation::Old);
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
        farField->StoreColoured(ColouredPointer(youngTarget, OneLoadBadRemap()));
        barrier.WriteReference(holder, *farField, youngTarget);
    }
    GC_EXPECT_TRUE(remembered.Contains(farSlot));

    LiveInfo* holderLive = fx.PlantLiveInfo(holderRegion);
    RegionBitmap* holderMarks = fx.PlantMarkBitmap<Generation::Old>(holderLive, holderRegion->GetRegionSize());
    (void)holderMarks->MarkBits(0, holder->GetMArraySize(), holderRegion->GetRegionSize());
    holderRegion->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LateBackfillState forwarding = PrepareLateBackfill(fx, collector, Generation::Young);
    farField->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    // Model the prior young relocate-start that makes a current old-remset
    // field load-bad. Major mark-start changes mark colours only; the true
    // Preforward entry must consume this remap-stale word.
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);

    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    ResetRemapYoungRootsTestReceipt(farSlot);

    collector.GetGenerationCycle(GCCycleGeneration::YOUNG).InitializeWorkers(2);
    collector.GetGenerationCycle(GCCycleGeneration::OLD).InitializeWorkers(2);
    // The real major driver prepares the mark engine before entering its body.
    collector.StartOldMarkWork();
    {
        DriverLocker driver(resources);
        collector.RunGarbageCollection(1, GC_REASON_USER);
    }

    const RemapYoungRootsTestReceipt receipt = ReadRemapYoungRootsTestReceipt();
    const bool holderNonAllocating = !holderRegion->IsAllocating();
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

#if defined(MRT_REMAP_YOUNG_ROOTS_RECEIPT_AVAILABLE)
// ZGenerationOld::remap_young_roots, zGeneration.cpp:1509: enter through
// the real major driver; a registered runtime mutator owns the raw root.
void RunMajorRawRemap(bool promoted, bool managed, bool oldPending = false, bool fallback = false, unsigned nestedKind = 0)
{
    ZStat::Initialize();
    GcHeapFixture& fx = ProductFixture();
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    LoadHealDeliveryTestAccess::PublishColours(collector);
    LateBackfillState forwarding {};
    BaseObject* secondOld = nullptr;
    if (oldPending) {
        RegionInfo* region = ResetDeliveryUnit(fx, 5);
        BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
        BaseObject* from = fx.PlaceObject(region->GetRegionStart() + dead->GetSize());
        region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
        LiveInfo* live = fx.PlantLiveInfo(region);
        (void)fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
        auto& regionManager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        RelocationReceiptTestAccess::ParkFrom(regionManager, region);
        // ZGC selects a set only when packing can release a page. Two sparse
        // pages are input to the real selector; a single page is exempted.
        RegionInfo* second = ResetDeliveryUnit(fx, 4);
        secondOld = fx.PlaceObject(second->GetRegionStart());
        second->SetRegionAllocPtr(reinterpret_cast<MAddress>(secondOld) + secondOld->GetSize());
        LiveInfo* secondLive = fx.PlantLiveInfo(second);
        (void)fx.PlantMarkBitmap<Generation::Old>(secondLive, second->GetRegionSize());
        RelocationReceiptTestAccess::ParkFrom(regionManager, second);
        forwarding = {region, region, from, dead, live, Generation::Old};
    } else {
        forwarding = PrepareLateBackfill(fx, collector, Generation::Young);
        LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    }
    std::unique_ptr<DeliverySharedPageScope> allocation;
    if (oldPending) {
        // Supply allocation capacity, as Heap::Init would. The product copier
        // still allocates, copies and publishes the destination itself.
        allocation = std::make_unique<DeliverySharedPageScope>(ResetDeliveryUnit(fx, 2));
    }
    if (promoted) {
        // The source table, not its current page generation, owns remapping.
        forwarding.region->SetYoungRegionFlag(0);
    }

    MutatorManager& manager = MutatorManager::Instance();
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(16) uintptr_t nestedStorage[8] {};
    RootSlot* nestedField = nullptr;
    BaseObject* rootInput = forwarding.from;
    if (nestedKind != 0) {
        mutator->SetStackTopAddr(reinterpret_cast<uintptr_t>(nestedStorage));
        mutator->SetStackSize(sizeof(nestedStorage));
        rootInput = reinterpret_cast<BaseObject*>(&nestedStorage[2]);
        if (nestedKind == 1) rootInput->SetClassInfo(fx.typeInfo);
        nestedField = &RootSlotAt(static_cast<void*>(&nestedStorage[nestedKind == 1 ? 3 : 2]));
        StorePlain(*nestedField, from_object(forwarding.from));
    }
    ObjectRef* root = mutator->AddNativeFrameRoot(rootInput);
    ObjectRef* secondRoot = secondOld == nullptr ? nullptr : mutator->AddNativeFrameRoot(secondOld);
    ObjectRef* nullRoot = mutator->AddNativeFrameRoot(nullptr);
    static uintptr_t nonHeapStorage[2] = {};
    ObjectRef* nonHeapRoot = mutator->AddNativeFrameRoot(reinterpret_cast<BaseObject*>(nonHeapStorage));
    uintptr_t frame[8] = {};
#if defined(__x86_64__) && defined(__linux__)
    if (managed) {
        InitializeDerivedBaseMap(false);
        frame[0] = reinterpret_cast<uintptr_t>(forwarding.from);
        frame[1] = frame[0] + 8;
        frame[2] = reinterpret_cast<uintptr_t>(derivedBaseMapImage.pc) + 9;
        auto& context = mutator->GetUnwindContext();
        context.frameInfo.mFrame.SetIP(derivedBaseMapImage.pc);
        context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&frame[3]));
        context.anchorFA = nullptr;
        mutator->SetManagedContext(true);
        if (fallback) {
            // Missing stack bounds is the product's explicit legacy fallback
            // case. The valid frame metadata and heap roots remain unchanged.
            mutator->SetStackTopAddr(0);
        }
    }
#endif
    ResetRemapYoungRootsTestReceipt(reinterpret_cast<uintptr_t>(forwarding.from));
    RuntimeWorkers threadPool(1u);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, &threadPool);
    collector.GetGenerationCycle(GCCycleGeneration::YOUNG).InitializeWorkers(2);
    collector.GetGenerationCycle(GCCycleGeneration::OLD).InitializeWorkers(2);
    // This fixture invokes the old body without the driver's young prelude.
    // Supply the product mark-start sequence event before publishing old roots.
    auto& oldCycle = collector.GetGenerationCycle(GCCycleGeneration::OLD);
    if (!oldCycle.Snapshot().active) oldCycle.Begin(0);
    GenerationSequenceFixture::Advance(oldCycle);
    collector.StartOldMarkWork();
    {
        DriverLocker driver(resources);
        collector.RunGarbageCollection(1, GC_REASON_USER);
    }
    const auto receipt = ReadRemapYoungRootsTestReceipt();
    const uintptr_t expected = oldPending
        ? ForwardingTable::FindTo(reinterpret_cast<uintptr_t>(forwarding.from), Generation::Old)
        : reinterpret_cast<uintptr_t>(forwarding.to);
    const uint64_t expectedVisits = managed ? 3 : 1; // native, ordinary base, derived temporary base
    const uintptr_t before = reinterpret_cast<uintptr_t>(forwarding.from);
    const bool result = receipt.visits == expectedVisits && receipt.before == before &&
        receipt.after == (oldPending ? before : expected) &&
        receipt.heals == (oldPending ? 0 : expectedVisits) &&
        (!oldPending || receipt.oldPendingVisits == expectedVisits) &&
        expected != 0 && (!oldPending || expected != before) &&
        (nestedField == nullptr ? raw(root->LoadPlain()) == expected :
            raw(root->LoadPlain()) == reinterpret_cast<uintptr_t>(rootInput) && raw(nestedField->LoadPlain()) == expected) &&
        is_null(nullRoot->LoadPlain()) && raw(nonHeapRoot->LoadPlain()) == reinterpret_cast<uintptr_t>(nonHeapStorage) &&
        (!managed || (frame[0] == expected && frame[1] == expected + 8));
    std::fprintf(stderr, "RAW_REMAP_TARGET_ASSERT promoted=%u managed=%u visits=%llu before=%zx after=%zx "
        "expected=%zx base=%zx derived=%zx old_pending=%llu final_root=%zx result=%u\n", unsigned(promoted), unsigned(managed),
        static_cast<unsigned long long>(receipt.visits), receipt.before, receipt.after,
        expected, frame[0], frame[1], static_cast<unsigned long long>(receipt.oldPendingVisits),
        raw(root->LoadPlain()), unsigned(result));
    if (nestedKind != 0) {
        std::fprintf(stderr, "NESTED_REMAP_TARGET kind=%u observed=%zx expected=%zx result=%u\n",
                     nestedKind, raw(nestedField->LoadPlain()), expected, unsigned(result));
    }
    GC_EXPECT_TRUE(result);
    mutator->RemoveNativeFrameRoot(root);
    if (secondRoot != nullptr) mutator->RemoveNativeFrameRoot(secondRoot);
    mutator->RemoveNativeFrameRoot(nullRoot);
    mutator->RemoveNativeFrameRoot(nonHeapRoot);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    RelocationReceiptTestAccess::BindRuntimeWorkers(resources, nullptr);
}
void CheckMajorRawRemap(bool promoted, bool managed, bool oldPending = false, bool fallback = false, unsigned nestedKind = 0)
{
    const AbortCapture outcome = CaptureAbort([&] { RunMajorRawRemap(promoted, managed, oldPending, fallback, nestedKind); });
    std::fprintf(stderr, "%s", outcome.output.c_str());
    const bool completed = WIFEXITED(outcome.status) && WEXITSTATUS(outcome.status) == 0 &&
        outcome.output.find("result=1") != std::string::npos;
    std::fprintf(stderr, "RAW_REMAP_OUTCOME_ASSERT promoted=%u managed=%u old=%u status=%d completed=%u\n",
        unsigned(promoted), unsigned(managed), unsigned(oldPending), outcome.status, unsigned(completed));
    GC_EXPECT_TRUE(completed);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorRemapsStackObjectField)
{
    CheckMajorRawRemap(false, false, false, false, 1);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorRemapsHeaderlessRecordField)
{
    CheckMajorRawRemap(false, false, false, false, 2);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkConsumesYoungTable)
{
    CheckMajorRawRemap(false, false);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkConsumesPromotedYoungSource)
{
    CheckMajorRawRemap(true, false);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorKeepsOldPendingThenRelocatesRawRoot)
{
    CheckMajorRawRemap(false, false, true);
}
#if defined(__x86_64__) && defined(__linux__)
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkRemapsDerivedYoungSource)
{
    CheckMajorRawRemap(false, true);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorWatermarkRemapsDerivedPromotedSource)
{
    CheckMajorRawRemap(true, true);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorKeepsOldPendingThenRelocatesDerivedRoot)
{
    CheckMajorRawRemap(false, true, true);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorFallbackRemapsDerivedPromotedSource)
{
    CheckMajorRawRemap(true, true, false, true);
}
GC_OTHER_VM_TEST(RawRemapYoungProduct, MajorFallbackKeepsOldPendingThenRelocates)
{
    CheckMajorRawRemap(false, true, true, true);
}
#endif
#endif

#include "b09_runtime_fixture.hpp"

static void CheckCompactIncoming(bool overlapping, bool external = false, bool major = false, bool flipYoung = false, bool rootBeforeCompact = false, bool exportEntry = false)
{
    B09RuntimeFixture runtime;
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    auto* region = fx.region0;
    const MAddress start = region->GetRegionStart();
    auto* dead = fx.PlaceObject(start);
    const size_t size = dead->GetSize();
    auto* first = fx.PlaceObject(start + size);
    auto* second = fx.PlaceObject(start + 2 * size);
    region->SetRegionAllocPtr(start + 3 * size);
    fx.region1->SetRegionAllocPtr(fx.region1->GetRegionEnd());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    LiveInfo* live = PrepareForwardable(fx, region, reinterpret_cast<MAddress>(first));
    (void)live->GetMarkFace().bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(second)), size, region->GetRegionSize());
    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, region);
    auto& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(region, reinterpret_cast<MAddress>(second));
    GC_EXPECT_TRUE(request.accepted);
    if (rootBeforeCompact) {
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
        collector.ResurrectExportObject(second);
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
        collector.ResurrectExportObject(second);
        LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    }
    manager.CompactRegion(region);
    region->MarkForwardingDone();
    (void)queue.Wait(request.request);
    auto* forwarding = request.request->page_forwarding();
    const MAddress firstTo = forwarding->find(start + size);
    const MAddress secondTo = forwarding->find(start + 2 * size);
    std::fprintf(stderr, "B09_OVERLAP_PRECONDITION size=%zu first_delta=%zu second_delta=%zu\n", size, firstTo-start, secondTo-start);
    GC_EXPECT_EQ(firstTo, start);
    GC_EXPECT_EQ(secondTo, start + size);
    auto* current = external ? fx.PlaceObject(fx.region1->GetRegionStart()) : reinterpret_cast<BaseObject*>(overlapping ? secondTo : firstTo);
    GC_EXPECT_TRUE(current->IsValidObject());
    if (!rootBeforeCompact) {
        collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
        if (exportEntry) {
            const U64 handle = Heap::GetHeap().RegisterExportRoot(current);
            Heap::GetHeap().CrossAccessBarrier(handle);
            collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
            Heap::GetHeap().CrossAccessBarrier(handle);
            Heap::GetHeap().RemoveExportObject(handle);
        } else {
            collector.ResurrectExportObject(current);
            collector.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
            collector.ResurrectExportObject(current);
        }
    }
    const bool identity = RelocationReceiptTestAccess::BothResurrectionSetsEqual(
        collector, rootBeforeCompact ? second : current);
    std::fprintf(stderr, "B09_OVERLAP_TARGET_ASSERT current_identity=%d\n", identity);
    GC_EXPECT_TRUE(identity);
    if (flipYoung) {
        LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    }
    const auto visited = major ? RelocationReceiptTestAccess::EnumMajorValueRoots(collector)
                               : RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
    const bool consumerIdentity = visited.size() == 2 &&
        std::all_of(visited.begin(), visited.end(), [current](BaseObject* p) { return p == current; }) &&
        RelocationReceiptTestAccess::BothResurrectionSetsEqual(collector, current);
    std::fprintf(stderr, "B09_CONSUMER_TARGET_ASSERT mode=%s count=%zu identity=%d\n",
                 major ? "major" : "minor", visited.size(), consumerIdentity);
    GC_EXPECT_TRUE(consumerIdentity);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, IncomingCurrentCompactDestinationKeepsIdentity)
{
    CheckCompactIncoming(true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, NonOverlappingCurrentDestinationKeepsIdentity)
{
    CheckCompactIncoming(false);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, ExternalCurrentDestinationKeepsIdentity)
{
    CheckCompactIncoming(false, true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorIncomingCurrentCompactDestinationKeepsIdentity)
{
    CheckCompactIncoming(true, false, true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorExternalCurrentDestinationKeepsIdentity)
{
    CheckCompactIncoming(false, true, true);
}

GC_TEST(ForwardingPublicationProduct, ResolveStoreValueAlreadyToStartRejectsNonUsable)
{
#if defined(__linux__)
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    PartialCompactState state = PreparePartialCompact(fx, collector, true);

    RegionManager manager;
    RelocationReceiptTestAccess::ParkFrom(manager, state.region);
    ForwardingTable::RetainPageOwner(state.region)->set_in_place();
    manager.CompactRegion(state.region);
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
    ForwardingTable::RetainPageOwner(state.region)->set_in_place();
    manager.CompactRegion(state.region);
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

GC_OTHER_VM_TEST(ValueRootCurrentization, MinorCurrentOldRootSurvivesYoungColorFlip)
{
    CheckCompactIncoming(true, false, false, true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorCurrentOldRootSurvivesYoungColorFlip)
{
    CheckCompactIncoming(true, false, true, true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MinorStoredCurrentRootRemapsAfterOldColorFlip)
{
    CheckCompactIncoming(true, false, false, false, true);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, MajorStoredCurrentRootRemapsAfterOldColorFlip)
{
    CheckCompactIncoming(true, false, true, false, true);
}

// ZPage::clone_for_promotion/reset (zPage.cpp:64,103): current page identity
// changes independently of the source forwarding retained for remapping.
GC_TEST(PageGeneration579, PromotionAndCarrierRouting)
{
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GcHeapFixture fixture;
    auto* region = fixture.region0;
    // PrepareForwardableRegion walks a dense allocation range from page start.
    fixture.obj0 = fixture.PlaceObject(region->GetRegionStart());
    region->SetRegionAllocPtr(region->GetRegionStart() + fixture.obj0->GetSize());
    region->SetYoungRegionFlag(1);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    RegionList selected("page579-selected");
    selected.PrependRegion(region, region->GetRegionType());
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Young, selected));
    (void)selected.TakeHeadRegion();
    RelocationReceiptTestAccess::PrepareProductPage<Generation::Young>(region);
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fixture.obj1);
    auto* forwarding = ForwardingTable::get(from, Generation::Young);
    GC_EXPECT_EQ(forwarding->insert(from, to), to);
    auto& collector = Heap::GetHeap().GetCollector();
    GC_EXPECT_TRUE(collector.ObjectGeneration(fixture.obj0) == Generation::Young);
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::young);
    auto original = region->CloneForPromotion(region->GetMarkView<Generation::Young>());
    const Generation current = collector.ObjectGeneration(fixture.obj0);
    std::fprintf(stderr, "PAGE579 promotion current=%u id=%u\n",
                 static_cast<unsigned>(current), static_cast<unsigned>(region->generation_id()));
    GC_EXPECT_TRUE(current == Generation::Old);
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::old);
    const auto retained = ForwardingTable::LookupTo(from, current);
    std::fprintf(stderr, "PAGE579 retained to=%zx expected=%zx\n", retained.to, to);
    GC_EXPECT_EQ(retained.to, to);
    ForwardingTable::ClearPageOwner(region);
    const auto cleared = ForwardingTable::LookupTo(from, current);
    std::fprintf(stderr, "PAGE579 cleared answer=%u\n", static_cast<unsigned>(cleared.answer));
    GC_EXPECT_TRUE(cleared.answer == ForwardingTable::ToAnswer::Unarmed);
    // The old source table still exists: clearing the carrier must not recover
    // its generation from stale page metadata or search both maps.
    GC_EXPECT_EQ(ForwardingTable::FindTo(from, Generation::Young), to);
    GC_EXPECT_TRUE(collector.ObjectGeneration(fixture.obj0) == Generation::Old);
}

GC_TEST(PageGeneration579, ResetAndReuseCurrentGeneration)
{
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    GcHeapFixture fixture;
    auto* region = fixture.region0;
    auto& collector = Heap::GetHeap().GetCollector();
    for (uint8_t young : {1, 0, 1, 0}) {
        region->SetYoungRegionFlag(young);
        const Generation expected = young ? Generation::Young : Generation::Old;
        const ZGenerationId expectedId = young ? ZGenerationId::young : ZGenerationId::old;
        const Generation current = collector.ObjectGeneration(fixture.obj0);
        std::fprintf(stderr, "PAGE579 reset young=%u current=%u id=%u\n", young,
                     static_cast<unsigned>(current), static_cast<unsigned>(region->generation_id()));
        GC_EXPECT_TRUE(current == expected);
        GC_EXPECT_TRUE(region->generation_id() == expectedId);
    }
    const auto oldLife = region->GetRegionLifeId();
    RegionInfo::RetirePage(region, [region]() { region->InitFreeUnits(); });
    region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region->GetRegionLifeId() != oldLife);
    GC_EXPECT_TRUE(region->generation_id() == ZGenerationId::old);
    GC_EXPECT_TRUE(collector.ObjectGeneration(fixture.obj0) == Generation::Old);
}

GC_OTHER_VM_TEST(ValueRootCurrentization, ExportEntryCurrentCompactDestinationKeepsIdentity)
{
    CheckCompactIncoming(true, false, false, false, false, true);
}
