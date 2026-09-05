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
#include "Heap/Verify/FromPageDetachCheck.h"
#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

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
        const ForwardingProvenance provenance{
            ForwardingHolderKind::HeapRef, forwarding, &from
        };
        return collector.WaitRoutedTipReady(from, to, forwarding, provenance);
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
        return collector.ForwardObjectImpl(from, copyPage);
    }

    static void RemapYoungRoots(WCollector& collector) { collector.RemapYoungRoots(); }
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
    static void Reset()
    {
        std::lock_guard<std::mutex> guard(mu);
        entered = false;
        released = false;
    }

    static void Hook(RegionInfo*, BaseObject*)
    {
        std::unique_lock<std::mutex> lock(mu);
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
    static bool entered;
    static bool released;
};

std::mutex CopyAdmissionBarrier::mu;
std::condition_variable CopyAdmissionBarrier::cv;
bool CopyAdmissionBarrier::entered = false;
bool CopyAdmissionBarrier::released = false;

struct CopyAdmissionWitness {
    static void Reset() { hits.store(0, std::memory_order_relaxed); }

    static void Hook(RegionInfo*, BaseObject*) { hits.fetch_add(1, std::memory_order_relaxed); }

    static uint32_t Hits() { return hits.load(std::memory_order_relaxed); }

    static std::atomic<uint32_t> hits;
};

std::atomic<uint32_t> CopyAdmissionWitness::hits { 0 };

using ProductSetCopyAdmissionTestHook = void (*)(void (*)(RegionInfo*, BaseObject*));

ProductSetCopyAdmissionTestHook ProductSetCopyAdmissionTestHookFn()
{
    void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        handle = dlopen("libcangjie-runtime.so", RTLD_NOW);
    }
    return handle == nullptr ? nullptr : reinterpret_cast<ProductSetCopyAdmissionTestHook>(
        dlsym(handle, "MRT_SetCopyAdmissionTestHook"));
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

void PinOwnerGeneration(RegionInfo* region, Generation gen)
{
    region->SetYoungRegionFlag(gen == Generation::Young ? 1 : 0);
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

    region->SetRouteInfo(destination->GetRegionStart(), static_cast<uint32_t>(from->GetSize()));
    region->SetRouteState(RegionInfo::RouteState::FORWARDED);
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

} // namespace

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
GC_TEST(ForwardingPublicationProduct, MutatorRuntimeEntryReachesCopyAdmission)
{
    ProductSetCopyAdmissionTestHook setCopyAdmissionHook = ProductSetCopyAdmissionTestHookFn();
    if (setCopyAdmissionHook == nullptr) {
        std::fprintf(stderr, "MUTATOR_ENTRY_TEST_NOT_RUN reason=HOOK_ABSENT\n");
        return;
    }

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
    region->SetRouteInfo(reinterpret_cast<MAddress>(expected), static_cast<uint32_t>(objectSize));
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& queue = productSpace.GetRegionManager().GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const MAddress fromAddress = reinterpret_cast<MAddress>(from);
    const auto request = queue.Add(region, fromAddress);
    GC_EXPECT_TRUE(request.accepted);

    CopyAdmissionWitness::Reset();
    setCopyAdmissionHook(&CopyAdmissionWitness::Hook);
    BaseObject* resolved = RelocationReceiptTestAccess::ProductRelocateOrRemap(
        collector, from, region->generation_id());
    setCopyAdmissionHook(nullptr);

    const bool published = request.request->state() == RelocationRequestQueue::State::COMPLETED;
    if (!published) {
        (void)queue.Fail(fromAddress);
    }
    const uint32_t admissionHits = CopyAdmissionWitness::Hits();
    const bool headerForwarded = from->IsForwarded();
    const MAddress receipt = ForwardingTable::FindTo(fromAddress);
    const int32_t copyCount = region->CopyInflight();
    const bool workersDone = queue.SynchronizePoll().workersDone;

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-mutator-entry");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);

    GC_EXPECT_EQ(admissionHits, 1u);
    GC_EXPECT_TRUE(resolved == expected);
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(headerForwarded);
    GC_EXPECT_EQ(receipt, reinterpret_cast<MAddress>(expected));
    GC_EXPECT_EQ(copyCount, 0);
    GC_EXPECT_TRUE(workersDone);
}

GC_TEST(ForwardingPublicationProduct, LateWaitBackfillCannotReopenSealedGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        (void)RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, state.from, state.to, state.region);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
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
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
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
    GC_EXPECT_FALSE(lookup.unavailable_route_state_valid());
    GC_EXPECT_EQ(lookup.unavailable_route_state(), static_cast<uint8_t>(0));

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
    GC_EXPECT_FALSE(noGhost.unavailable_route_state_valid());
    GC_EXPECT_EQ(noGhost.unavailable_route_state(), static_cast<uint8_t>(0));
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
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(from)) == region);
    GC_EXPECT_FALSE(from->IsForwarded());

    const ForwardingTable::LookupResult result =
        ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    region->DispelGhostFromRegion();
    from->SetStateCode(ObjectState::FORWARDED);

    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_TRUE(RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(from)) == nullptr);
    GC_EXPECT_TRUE(result.answer == ForwardingTable::ToAnswer::Unavailable);
    GC_EXPECT_TRUE((static_cast<uint8_t>(result.unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::PublicationClosed)) != 0);
    GC_EXPECT_TRUE((static_cast<uint8_t>(result.unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::TableDestroyed)) != 0);
    GC_EXPECT_FALSE(result.activeCandidate);
    GC_EXPECT_TRUE(result.activeAnswer == ForwardingTable::ToAnswer::Unarmed);
    GC_EXPECT_TRUE(result.retiredAnswer == ForwardingTable::ToAnswer::Unarmed);
    GC_EXPECT_TRUE(result.publicationClosed);

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

    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    RegionManager manager;
    RelocationReceiptTestAccess::Exempt(manager, region);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-kept-active-receipt");
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)),
                 reinterpret_cast<MAddress>(to));
    GC_EXPECT_TRUE(RelocationReceiptTestAccess::ProductFindToVersion(collector, from).found() == to);

    from->SetStateCode(ObjectState::NORMAL);
    ForwardingTable::ReclaimRetired("gc-unit-kept-active-receipt-cleanup");
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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

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
    region->SetRouteState(RegionInfo::RouteState::FORWARDED);

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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
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
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)), 0);
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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    region->ClearGhostRegionBit();
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    ForwardingTable::Publication activePublication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, reinterpret_cast<MAddress>(from));
    GC_EXPECT_EQ(ForwardingTable::InstallMapping(activePublication, reinterpret_cast<MAddress>(from),
                                                 reinterpret_cast<MAddress>(newTo)).address,
                 reinterpret_cast<MAddress>(newTo));
    activePublication = ForwardingTable::Publication();

    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_TRUE(from->IsForwarded());
    GC_EXPECT_EQ(ForwardingTable::FindRetiredTo(reinterpret_cast<MAddress>(from)), 0);
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
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
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
    GC_EXPECT_NE(result.unavailable_table_id(), static_cast<uintptr_t>(0));

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
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
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
    state.region->SetRouteState(RegionInfo::RouteState::COMPACTED);

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
    state.region->SetRouteState(RegionInfo::RouteState::COMPACTED);

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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
    region->MarkForwardingDone();
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(lookup.to == 0);
    GC_EXPECT_TRUE(lookup.answer != ForwardingTable::ToAnswer::ArmedHit);

    BaseObject* holder = fx.obj0;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    Barrier barrier(collector, Heap::GetHeap().GetRememberedSet());
    AbortCapture aborted = CaptureAbort([&]() { barrier.WriteReference(holder, field, from); });
    if (!WIFSIGNALED(aborted.status)) {
        std::fprintf(stderr, "WAIT_PROVENANCE status=%d output=\n%s\n", aborted.status,
                     aborted.output.c_str());
    }
    GC_EXPECT_TRUE(WIFSIGNALED(aborted.status));
    GC_EXPECT_EQ(WTERMSIG(aborted.status), SIGABRT);
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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
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
    (void)queue.Fail(reinterpret_cast<MAddress>(from));
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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
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
    region->SetRouteState(RegionInfo::RouteState::COMPACTED);
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

    const MAddress receipt = queue.Wait(request.request);
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
    state.region->SetRouteState(RegionInfo::RouteState::COMPACTED);

    const MAddress receipt = queue.Wait(request.request);
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
    buffer->SetRegion(routeDestination);
    GC_EXPECT_TRUE(manager.RouteRegion(region));
    GC_EXPECT_TRUE(region->GetRouteState() == RegionInfo::RouteState::ROUTED);
    buffer->ClearRegion();
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);

    // retain refusal means a worker owns the forwarding. ZGC queues the
    // request and waits; it does not copy without a retain token
    // (zRelocate.cpp:382-410).
    const auto seeded = queue.Add(region, from);
    BaseObject* resolved = nullptr;
    std::thread waiter([&]() {
        resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, liveObject, nullptr, region);
    });
    RelocationRequestQueue::Handle claimed = queue.PruneAndClaim();
    BaseObject* workerResult = RelocationReceiptTestAccess::TryForward(collector, liveObject);
    (void)manager.CompleteRelocationRequests(region);
    const bool workerClosed = queue.SynchronizePoll().workersDone;
    waiter.join();

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

GC_TEST(ForwardingPublicationProduct, CompletedReceiptResolvesWithoutForwardingTableLookup)
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
    buffer->SetRegion(routeDestination);
    GC_EXPECT_TRUE(manager.RouteRegion(region));
    buffer->ClearRegion();
    RelocationRequestQueue& queue = manager.GetRelocationRequestQueue();
    queue.BeginWorkers(1);

    // A ROUTED page with no receipt is completed by the registered worker;
    // the waiting visitor consumes only the installed receipt.
    const auto seeded = queue.Add(region, from);
    BaseObject* resolved = nullptr;
    std::thread waiter([&]() {
        resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, fromObject, nullptr, region);
    });
    RelocationRequestQueue::Handle claimed = queue.PruneAndClaim();
    BaseObject* workerResult = RelocationReceiptTestAccess::TryForward(collector, fromObject);
    (void)manager.CompleteRelocationRequests(region);
    const bool workerClosed = queue.SynchronizePoll().workersDone;
    waiter.join();

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

    const MAddress resolved = queue.Wait(request.request);
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
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
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

    ForwardingTable::PublishMarkCoverage(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    GC_EXPECT_TRUE((static_cast<uint8_t>(ForwardingTable::LookupTo(from).unavailableCause) &
                    static_cast<uint8_t>(ForwardingTable::ToUnavailableCause::TableDestroyed)) != 0);

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::ArmedHit);
    ForwardingTable::PublishMarkCoverage(Generation::Old);
    ForwardingTable::ReclaimRetired("old-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    ForwardingTable::PublishMarkCoverage(Generation::Old);
    ForwardingTable::ReclaimRetired("old-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::ArmedHit);
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    {
        ForwardingTable::Publication reader = ForwardingTable::RetainCovering(from);
        GC_EXPECT_TRUE(static_cast<bool>(reader));
        ForwardingTable::ReclaimRetired("young-mark-coverage");
        GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, to);
    }
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from).to, 0);
    GC_EXPECT_TRUE(ForwardingTable::LookupTo(from).answer == ForwardingTable::ToAnswer::Unavailable);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    const uint64_t before = ForwardingTable::MarkCoverageEpoch(Generation::Young);
    GC_EXPECT_TRUE(before < required);
    ForwardingTable::ReclaimRetired("old-remap-young-roots-complete");
    GC_EXPECT_EQ(ForwardingTable::MarkCoverageEpoch(Generation::Young), before);
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    const uint64_t after = ForwardingTable::MarkCoverageEpoch(Generation::Young);
    GC_EXPECT_TRUE(after > before);
    GC_EXPECT_TRUE(after >= required);
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
    ForwardingTable::PublishMarkCoverage(Generation::Young);
    ForwardingTable::ReclaimRetired("young-mark-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
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
    while (heldTable != nullptr && !heldTable->claimed().load(std::memory_order_acquire)) {
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

// zForwarding.cpp:134-169: claim inverts a positive refcount before waiting for
// outstanding Publication owners.  Observe that state transition rather than a
// timing window: correct clear reaches a negative count while the owner is held;
// a non-draining clear returns with a non-negative count.
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
    while (heldTable != nullptr && heldTable->ref_count().load(std::memory_order_acquire) >= 0 &&
           !clearDone.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool enteredClaimedDrain =
        heldTable != nullptr && heldTable->ref_count().load(std::memory_order_acquire) < 0;

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
    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& queue = productSpace.GetRegionManager().GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const auto request = queue.Add(region, from);
    GC_EXPECT_TRUE(request.accepted);

    StateWord oldWord = fromObject->GetStateWord();
    GC_EXPECT_TRUE(fromObject->TryLockObject(oldWord));
    GC_EXPECT_TRUE(region->NoteCopyInflight());
    BaseObject* relocated =
        RelocationReceiptTestAccess::ForwardExclusive(collector, fromObject, toObject, region);

    const bool productPublished = request.request->state() == RelocationRequestQueue::State::COMPLETED;
    GC_EXPECT_TRUE(productPublished);
    if (!productPublished) {
        (void)queue.Fail(from);
    }
    GC_EXPECT_TRUE(relocated == toObject);
    GC_EXPECT_EQ(request.request->receipt(), to);
    GC_EXPECT_EQ(queue.Wait(request.request), to);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), to);
    GC_EXPECT_TRUE(fromObject->IsForwarded());
    GC_EXPECT_EQ(region->CopyInflight(), 0);
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// The deterministic barrier is inside the product ForwardObjectImpl after the
// object lock is acquired and ENTERING is published, but before the copier
// count is committed. DrainScope must wait through that interval, then through
// the real CopyObject/receipt/Unlock path. Once sealed, a second real entry is
// refused and rolls its object header back without changing the count.
GC_TEST(ForwardingPublicationProduct, CopyAdmissionSealWaitsRealCopierAndRejectsLateEntry)
{
    ProductSetCopyAdmissionTestHook setCopyAdmissionHook = ProductSetCopyAdmissionTestHookFn();
    if (setCopyAdmissionHook == nullptr) {
        std::fprintf(stderr, "COPY_ADMISSION_TEST_NOT_RUN reason=HOOK_ABSENT\n");
        return;
    }

    GcHeapFixture& fx = ProductFixture();
    RegionInfo* region = RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    RegionInfo* destination = RegionInfo::InitRegion(3, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(region != nullptr && destination != nullptr);
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);

    BaseObject* serial = fx.PlaceObject(region->GetRegionStart() + 64);
    const size_t objectSize = serial->GetSize();
    BaseObject* first = fx.PlaceObject(reinterpret_cast<MAddress>(serial) + objectSize);
    BaseObject* late = fx.PlaceObject(reinterpret_cast<MAddress>(first) + objectSize);
    BaseObject* serialTo = fx.PlaceObject(destination->GetRegionStart() + 64);
    BaseObject* firstTo = fx.PlaceObject(reinterpret_cast<MAddress>(serialTo) + objectSize);
    BaseObject* lateTo = fx.PlaceObject(reinterpret_cast<MAddress>(firstTo) + objectSize);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(late) + objectSize);
    destination->SetRegionAllocPtr(reinterpret_cast<MAddress>(lateTo) + objectSize);

    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t serialOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(serial));
    const size_t firstOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(first));
    const size_t lateOffset = region->GetAddressOffset(reinterpret_cast<MAddress>(late));
    (void)bitmap->MarkBits(serialOffset, objectSize, region->GetRegionSize());
    (void)bitmap->MarkBits(firstOffset, objectSize, region->GetRegionSize());
    (void)bitmap->MarkBits(lateOffset, objectSize, region->GetRegionSize());
    region->AddLiveByteCount(3 * objectSize);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->RecordRouteStart(serialOffset);
    region->RecordRouteStart(firstOffset);
    region->RecordRouteStart(lateOffset);
    region->SetRouteInfo(reinterpret_cast<MAddress>(serialTo), static_cast<uint32_t>(3 * objectSize));
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& queue = productSpace.GetRegionManager().GetRelocationRequestQueue();
    queue.BeginWorkers(1);
    const MAddress serialAddress = reinterpret_cast<MAddress>(serial);
    const auto serialRequest = queue.Add(region, serialAddress);
    GC_EXPECT_TRUE(serialRequest.accepted);

    CopyAdmissionBarrier::Reset();
    setCopyAdmissionHook(&CopyAdmissionBarrier::Hook);
    BaseObject* serialResult = nullptr;
    std::thread serialCopier([&]() {
        serialResult = RelocationReceiptTestAccess::ForwardImpl(collector, serial, region);
    });
    CopyAdmissionBarrier::WaitEntered();
    const auto serialStateInAdmission = region->CopyAdmission();
    CopyAdmissionBarrier::Release();
    serialCopier.join();
    const bool serialPublished =
        serialRequest.request->state() == RelocationRequestQueue::State::COMPLETED;
    if (!serialPublished) {
        (void)queue.Fail(serialAddress);
    }
    const auto serialStateAfterCopy = region->CopyAdmission();
    const int32_t serialCountAfterCopy = region->CopyInflight();
    const bool serialHeaderForwarded = serial->IsForwarded();

    const MAddress firstAddress = reinterpret_cast<MAddress>(first);
    const auto request = queue.Add(region, firstAddress);
    GC_EXPECT_TRUE(request.accepted);

    CopyAdmissionBarrier::Reset();
    BaseObject* firstResult = nullptr;
    std::thread copier([&]() {
        firstResult = RelocationReceiptTestAccess::ForwardImpl(collector, first, region);
    });
    CopyAdmissionBarrier::WaitEntered();

    const bool objectLockedInGap = first->GetStateWord().IsLockedWord();
    const auto stateInGap = region->CopyAdmission();
    const int32_t countInGap = region->CopyInflight();
    std::atomic<bool> drainStarted{ false };
    std::atomic<bool> drainAcquired{ false };
    std::atomic<bool> allowWipe{ false };
    std::atomic<bool> wipeDone{ false };
    std::atomic<bool> drainDone{ false };
    std::thread drainer([&]() {
        drainStarted.store(true, std::memory_order_release);
        {
            FromPageDetach::ReusePermitScope reusePermit;
            RegionInfo::DrainScope drain(region, MutatorRelocate::Retire::TAKE_GARBAGE);
            drainAcquired.store(true, std::memory_order_release);
            while (!allowWipe.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            RegionInfo::ClearUnits(
                region->GetUnitIdx(), region->GetUnitCount(), FillerZeroDiag::Site::TAKE_GARBAGE);
            wipeDone.store(true, std::memory_order_release);
        }
        drainDone.store(true, std::memory_order_release);
    });
    while (!drainStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool drainReturnedInGap = drainAcquired.load(std::memory_order_acquire);

    CopyAdmissionBarrier::Release();
    copier.join();
    const bool firstPublished =
        request.request->state() == RelocationRequestQueue::State::COMPLETED;
    if (!firstPublished) {
        (void)queue.Fail(firstAddress);
    }
    while (!drainAcquired.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool firstHeaderForwarded = first->IsForwarded();
    const int32_t countAfterDrain = region->CopyInflight();
    const auto stateAfterDrain = region->CopyAdmission();

    // This call starts from the same real product entry. It acquires the
    // second object's lock, observes SEALED, rolls back NORMAL, and never
    // reaches CopyObject or the test hook.
    const int32_t countBeforeLate = region->CopyInflight();
    BaseObject* lateResult = reinterpret_cast<BaseObject*>(1);
    if (!drainReturnedInGap) {
        lateResult = RelocationReceiptTestAccess::ForwardImpl(collector, late, region);
    }
    const int32_t countAfterLate = region->CopyInflight();
    const auto lateHeader = late->GetStateWord().GetStateCode();

    allowWipe.store(true, std::memory_order_release);
    drainer.join();
    setCopyAdmissionHook(nullptr);

    const bool workersDone = queue.SynchronizePoll().workersDone;
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);

    GC_EXPECT_TRUE(serialStateInAdmission == ZForwardingLife::CopyAdmissionState::ENTERING);
    GC_EXPECT_TRUE(serialPublished);
    GC_EXPECT_TRUE(serialResult == serialTo);
    GC_EXPECT_TRUE(serialHeaderForwarded);
    GC_EXPECT_TRUE(serialStateAfterCopy == ZForwardingLife::CopyAdmissionState::OPEN);
    GC_EXPECT_EQ(serialCountAfterCopy, 0);
    GC_EXPECT_TRUE(objectLockedInGap);
    GC_EXPECT_TRUE(stateInGap == ZForwardingLife::CopyAdmissionState::ENTERING);
    GC_EXPECT_EQ(countInGap, 0);
    GC_EXPECT_FALSE(drainReturnedInGap);
    GC_EXPECT_TRUE(drainDone.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(firstPublished);
    GC_EXPECT_TRUE(firstResult == firstTo);
    GC_EXPECT_EQ(request.request->receipt(), reinterpret_cast<MAddress>(firstTo));
    GC_EXPECT_TRUE(firstHeaderForwarded);
    GC_EXPECT_EQ(countAfterDrain, 0);
    GC_EXPECT_TRUE(stateAfterDrain == ZForwardingLife::CopyAdmissionState::SEALED);
    GC_EXPECT_TRUE(lateResult == nullptr);
    GC_EXPECT_TRUE(lateHeader == ObjectState::NORMAL);
    GC_EXPECT_EQ(countBeforeLate, countAfterLate);
    GC_EXPECT_EQ(countAfterLate, 0);
    GC_EXPECT_TRUE(wipeDone.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(workersDone);
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
    const size_t recorded = manager.RecordPinnedCrossGenEdges();
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

// Product route: ResolveStoreValue -> ColourResolvedRefField -> HealSlot.  The
// producer assertion is the final address; the consumer assertion is the
// current store-good colour installed in the actual heap slot.
GC_TEST(LoadHealDeliveryProduct, RemapYoungRootsResolvesRecoloursAndHealsSlot)
{
    LoadHealDeliveryRuntime::Ensure();
    GcHeapFixture& fx = ProductFixture();
    RegionInfo* holderRegion = ResetDeliveryUnit(fx, 0);
    RegionInfo* youngRegion = ResetDeliveryUnit(fx, 1);
    youngRegion->SetYoungRegionFlag(1);
    youngRegion->SetYoungAge(1);
    BaseObject* holder = fx.PlaceObject(holderRegion->GetRegionStart());
    BaseObject* youngTarget = fx.PlaceObject(youngRegion->GetRegionStart());
    holderRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(holder) + holder->GetSize());
    youngRegion->SetRegionAllocPtr(reinterpret_cast<MAddress>(youngTarget) + youngTarget->GetSize());
    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);

    RememberedSet& remembered = DeliveryRememberedSet(fx);
    EmptyBothRememberedFaces(remembered);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LoadHealDeliveryTestAccess::PublishColours(collector);
    Barrier barrier(collector, remembered);
    {
        DeliveryNoAllocBufferScope directRemember;
        field->StoreColoured(zpointer::null);
        barrier.WriteReference(holder, *field, youngTarget);
    }
    GC_EXPECT_TRUE(remembered.Contains(slot));

    LateBackfillState forwarding = PrepareLateBackfill(fx, collector);
    // Publish a legal current word first.  The relocate-start epoch changes make that same
    // heap word double-bad without ever publishing a forbidden colour.
    field->StoreColoured(GcUnit::StoreGoodPointer(forwarding.from));
    GC_EXPECT_TRUE(collector.is_store_good(*field));
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    const uintptr_t doubleBad = LoadHealDeliveryTestAccess::DoubleBadColour(collector);
    GC_EXPECT_TRUE(doubleBad != 0 && (doubleBad & (doubleBad - 1)) == 0);
    GC_EXPECT_EQ(raw(field->GetFieldValue()) & REMAP_COLOUR_MASK, doubleBad);
    const uintptr_t before = raw(field->GetFieldValue());
    LoadHealDeliveryTestAccess::RemapYoungRoots(collector);
    const uintptr_t after = raw(field->GetFieldValue());
    const BaseObject* healedTarget = to_object(field->GetTargetObject());
    const bool addressResolved = healedTarget == forwarding.to;
    const bool colourInstalled = collector.is_store_good(*field);
    std::fprintf(stderr,
                 "DETAIL loadheal_remap before=0x%zx after=0x%zx from=%p expected_to=%p "
                 "actual_to=%p address_resolved=%u store_good=%u\n",
                 static_cast<size_t>(before), static_cast<size_t>(after), forwarding.from,
                 forwarding.to, healedTarget, static_cast<unsigned>(addressResolved),
                 static_cast<unsigned>(colourInstalled));
    std::fflush(stderr);

    GC_EXPECT_TRUE(addressResolved);
    GC_EXPECT_TRUE(colourInstalled);
    GC_EXPECT_NE(before, after);
    field->StoreColoured(zpointer::null);
    EmptyBothRememberedFaces(remembered);
    LoadHealDeliveryTestAccess::FlipOldRelocateStart(collector);
    LoadHealDeliveryTestAccess::FlipYoungRelocateStart(collector);
    CleanupLateBackfill(fx, forwarding);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    youngRegion->SetYoungRegionFlag(0);
}
