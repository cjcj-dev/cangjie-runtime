// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void ParkFrom(RegionManager& manager, RegionInfo* region)
    {
        manager.fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    }

    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static BaseObject* WaitRoutedTipReady(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* forwarding)
    {
        return collector.WaitRoutedTipReady(from, to, forwarding);
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

    static BaseObject* ForwardExclusive(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* copyPage)
    {
        return collector.ForwardObjectExclusive(from, to, copyPage);
    }
};

} // namespace MapleRuntime

namespace {

GcHeapFixture& ProductFixture()
{
    static GcHeapFixture fixture;
    static const bool initialized = ForwardingTable::Initialize(
        fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    GC_EXPECT_TRUE(initialized);
    return fixture;
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
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    ForwardingTable::DropRetiredCovering(state.region->GetRegionStart(), state.region->GetRegionSize());
    state.from->SetStateCode(ObjectState::NORMAL);
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
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());
    ForwardingTable::DropRetiredCovering(state.region->GetRegionStart(), state.region->GetRegionSize());
    if (state.region->IsGhostFromRegion()) {
        state.region->DispelGhostFromRegion();
    }
    state.region->metadata.liveInfo = nullptr;
    fx.FreePlanted(state.live);
}

} // namespace

GC_TEST(ForwardingPublicationProduct, LateWaitBackfillCannotReopenSealedGeneration)
{
    GcHeapFixture& fx = ProductFixture();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    LateBackfillState state = PrepareLateBackfill(fx, collector);
    ForwardingTable::ClearEntries(state.region->GetRegionStart(), state.region->GetRegionSize());

    BaseObject* resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
        collector, state.from, state.to, state.region);
    GC_EXPECT_TRUE(resolved == nullptr);
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
    // The sealed generation still carries a FORWARDED from-object but has no
    // mapping. ZGC treats this as an assertion state, not an ordinary miss.
    GC_EXPECT_TRUE(resolved.state() == FindToVersionResult::State::Unavailable);
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
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
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
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());

    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(0));
    FindToVersionResult result = RelocationReceiptTestAccess::ProductFindToVersion(collector, from);
    GC_EXPECT_TRUE(result.state() == FindToVersionResult::State::Unavailable);
    GC_EXPECT_EQ(ForwardingTable::ArmedMissCount(), static_cast<uint64_t>(0));
    GC_EXPECT_EQ(ForwardingTable::UnavailableCount(), static_cast<uint64_t>(1));

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
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
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

    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
    GC_EXPECT_FALSE(ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize()));
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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

    const MAddress receipt = queue.Wait(request.request);
    GC_EXPECT_EQ(receipt, expected);
    GC_EXPECT_TRUE(receipt != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), expected);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(expected)->IsValidObject());
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    CleanupPartialCompact(fx, state);
}

GC_TEST(ForwardingPublicationProduct, PageWaitThenLookupReadsOriginalCompactReceipt)
{
    GcHeapFixture& fx = ProductFixture();
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

    std::atomic<bool> waiterReturned{ false };
    BaseObject* resolved = nullptr;
    std::thread waiter([&]() {
        resolved = RelocationReceiptTestAccess::WaitRoutedTipReady(
            collector, liveObject, nullptr, region);
        waiterReturned.store(true, std::memory_order_release);
    });
    JoinGuard waiterGuard(waiter);
    while (queue.PendingCount() == 0 && !waiterReturned.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const bool enteredProductQueue = queue.PendingCount() == 1;
    GC_EXPECT_TRUE(enteredProductQueue);

    if (enteredProductQueue) {
        manager.CompactRegion(region);
        // Publish(from, receipt) notifies this request, but WaitUntil must not
        // return until the independent page predicate becomes true.
        GC_EXPECT_FALSE(waiterReturned.load(std::memory_order_acquire));
        region->MarkForwardingDone();
    }
    waiter.join();

    GC_EXPECT_TRUE(resolved == reinterpret_cast<BaseObject*>(expected));
    GC_EXPECT_TRUE(resolved != liveObject);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), expected);
    GC_EXPECT_TRUE(queue.SynchronizePoll().workersDone);

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    routeDestination->SetRouteDestHold(0);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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

    const MAddress resolved = queue.Wait(request.request);
    GC_EXPECT_EQ(resolved, start);
    GC_EXPECT_TRUE(resolved != from);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), resolved);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(resolved)->IsValidObject());

    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
    ForwardingTable::ClearEntries(start, region->GetRegionSize());
    ForwardingTable::DropRetiredCovering(start, region->GetRegionSize());
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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

    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
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

    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
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
    region->NoteCopyInflight();
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
    ForwardingTable::DropRetiredCovering(region->GetRegionStart(), region->GetRegionSize());
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
