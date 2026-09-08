// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static BaseObject* ForwardImpl(WCollector& collector, BaseObject* from, RegionInfo* page)
    {
        return collector.ForwardObjectImpl(from, page);
    }
};

} // namespace MapleRuntime

namespace {

using SetCopyAdmissionHook = void (*)(void (*)(RegionInfo*, BaseObject*));
using SetLockedWaiterHook = void (*)(void (*)(BaseObject*));

template<class Function>
Function ProductFunction(const char* name)
{
    void* handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    GC_EXPECT_TRUE(handle != nullptr);
    void* symbol = handle == nullptr ? nullptr : dlsym(handle, name);
    GC_EXPECT_TRUE(symbol != nullptr);
    Dl_info info {};
    GC_EXPECT_TRUE(symbol != nullptr && dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
                   std::strstr(info.dli_fname, "libcangjie-runtime.so") != nullptr);
    return reinterpret_cast<Function>(symbol);
}

BaseObject* ProductRelocateOrRemap(WCollector& collector, BaseObject* from, ZGenerationId generation)
{
    using ProductFn = BaseObject* (*)(const WCollector*, BaseObject*, ZGenerationId);
    ProductFn function = ProductFunction<ProductFn>(
        "_ZNK12MapleRuntime10WCollector24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdE");
    return function(&collector, from, generation);
}

struct LazyFixture {
    LazyFixture()
        : from(fx.obj0), destination(fx.region1),
          collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources())
    {
        fromPage = fx.region0;
        destination = RegionInfo::InitRegion(
            1, 1, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        GC_EXPECT_TRUE(destination != nullptr);
        fromAddress = reinterpret_cast<MAddress>(from);
        objectSize = RegionSpace::GetAllocSize(*from);
        destination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        destination->SetRegionAllocPtr(destination->GetRegionStart());

        fromPage->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        live = fx.PlantLiveInfo(fromPage);
        RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, fromPage->GetRegionSize());
        const size_t offset = fromPage->GetAddressOffset(fromAddress);
        (void)bitmap->MarkBits(offset, from->GetSize(), fromPage->GetRegionSize());
        fromPage->AddLiveByteCount(from->GetSize());
        fromPage->PrepareForwardableRegion(fromPage->GetMarkView<Generation::Old>());

        // Deliberately leave this exact start out of the geometric route table.
        // The product planner therefore returns null while the page itself is a
        // valid ROUTED relocation-set member: the real lazy-allocation premise.
        fromPage->SetRouteState(RegionInfo::RouteState::ROUTED);
        collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
        buffer = AllocBuffer::GetOrCreateAllocBuffer();
        buffer->ClearRegion();
        buffer->SetRegion(destination);
        AllocBuffer::ForceRelocationAllocationFailureForTest(false);
        AllocBuffer::ResetAllocationPathCountersForTest();
    }

    ~LazyFixture()
    {
        AllocBuffer::ForceRelocationAllocationFailureForTest(false);
        if (buffer != nullptr) {
            buffer->ClearRegion();
        }
        collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
        RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);
        fromPage->metadata.liveInfo = nullptr;
        fx.FreePlanted(live);
    }

    GcHeapFixture fx;
    RegionInfo* fromPage = nullptr;
    RegionInfo* destination = nullptr;
    BaseObject* from = nullptr;
    MAddress fromAddress = 0;
    size_t objectSize = 0;
    LiveInfo* live = nullptr;
    WCollector collector;
    AllocBuffer* buffer = nullptr;
};

struct AdmissionBarrier {
    static void Reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        entered = false;
        release = false;
    }

    static void Hook(RegionInfo*, BaseObject*)
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, []() { return release; });
    }

    static void WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, []() { return entered; });
    }

    static void Release()
    {
        std::lock_guard<std::mutex> guard(mutex);
        release = true;
        condition.notify_all();
    }

    static std::mutex mutex;
    static std::condition_variable condition;
    static bool entered;
    static bool release;
};

std::mutex AdmissionBarrier::mutex;
std::condition_variable AdmissionBarrier::condition;
bool AdmissionBarrier::entered = false;
bool AdmissionBarrier::release = false;

struct LockedWaiterWitness {
    static void Reset() { hits.store(0, std::memory_order_relaxed); }
    static void Hook(BaseObject*) { hits.fetch_add(1, std::memory_order_release); }
    static uint32_t Hits() { return hits.load(std::memory_order_acquire); }
    static std::atomic<uint32_t> hits;
};

std::atomic<uint32_t> LockedWaiterWitness::hits { 0 };

} // namespace

GC_OTHER_VM_TEST(LazyRelocationDestination, SuccessfulProductEntryOwnsOneCompleteDestination)
{
    LazyFixture fixture;
    const MAddress before = fixture.destination->GetRegionAllocPtr();
    BaseObject* resolved = ProductRelocateOrRemap(
        fixture.collector, fixture.from, fixture.fromPage->generation_id());
    const MAddress after = fixture.destination->GetRegionAllocPtr();
    const size_t ordinaryCalls = AllocBuffer::OrdinaryAllocationCallsForTest();
    const size_t relocationCalls = AllocBuffer::RelocationAllocationCallsForTest();

    std::fprintf(stderr,
                 "LAZY_RELOCATION_TARGET_ASSERTION_REACHED case=success before=%#zx after=%#zx ordinary=%zu relocation=%zu\n",
                 static_cast<size_t>(before), static_cast<size_t>(after), ordinaryCalls, relocationCalls);
    GC_EXPECT_TRUE(resolved == reinterpret_cast<BaseObject*>(before));
    GC_EXPECT_EQ(after, before + fixture.objectSize);
    GC_EXPECT_TRUE(resolved != nullptr && resolved->IsValidObject());
    GC_EXPECT_EQ(ForwardingTable::FindTo(fixture.fromAddress), before);
    GC_EXPECT_TRUE(fixture.from->IsForwarded());
    GC_EXPECT_EQ(ordinaryCalls + relocationCalls, 1u);
}

GC_OTHER_VM_TEST(LazyRelocationDestination, PublicationRejectAllocatesNothing)
{
    LazyFixture fixture;
    const MAddress before = fixture.destination->GetRegionAllocPtr();
    ForwardingTable::ForcePublicationClosedForTest(fixture.fromAddress);
    BaseObject* resolved = RelocationReceiptTestAccess::ForwardImpl(
        fixture.collector, fixture.from, fixture.fromPage);
    const MAddress after = fixture.destination->GetRegionAllocPtr();

    std::fprintf(stderr,
                 "LAZY_RELOCATION_TARGET_ASSERTION_REACHED case=publication-reject before=%#zx after=%#zx ordinary=%zu relocation=%zu\n",
                 static_cast<size_t>(before), static_cast<size_t>(after),
                 AllocBuffer::OrdinaryAllocationCallsForTest(),
                 AllocBuffer::RelocationAllocationCallsForTest());
    GC_EXPECT_TRUE(resolved == nullptr);
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_TRUE(fixture.from->GetStateWord().GetStateCode() == ObjectState::NORMAL);
    GC_EXPECT_EQ(AllocBuffer::OrdinaryAllocationCallsForTest(), 0u);
    GC_EXPECT_EQ(AllocBuffer::RelocationAllocationCallsForTest(), 0u);
}

GC_OTHER_VM_TEST(LazyRelocationDestination, ConcurrentLoserConsumesNoSecondDestination)
{
    LazyFixture fixture;
    RegionInfo* secondDestination = RegionInfo::InitRegion(
        2, 1, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    GC_EXPECT_TRUE(secondDestination != nullptr);
    secondDestination->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    secondDestination->SetRegionAllocPtr(secondDestination->GetRegionStart());

    SetCopyAdmissionHook setAdmission =
        ProductFunction<SetCopyAdmissionHook>("MRT_SetCopyAdmissionTestHook");
    SetLockedWaiterHook setWaiter =
        ProductFunction<SetLockedWaiterHook>("MRT_SetLockedRelocationWaiterTestHook");
    AdmissionBarrier::Reset();
    LockedWaiterWitness::Reset();
    setAdmission(&AdmissionBarrier::Hook);
    setWaiter(&LockedWaiterWitness::Hook);

    BaseObject* first = nullptr;
    BaseObject* second = nullptr;
    std::thread winner([&]() {
        AllocBuffer::GetOrCreateAllocBuffer()->SetRegion(fixture.destination);
        first = ProductRelocateOrRemap(fixture.collector, fixture.from, fixture.fromPage->generation_id());
        AllocBuffer::GetOrCreateAllocBuffer()->ClearRegion();
    });
    JoinGuard winnerGuard(winner);
    AdmissionBarrier::WaitEntered();
    std::thread loser([&]() {
        AllocBuffer::GetOrCreateAllocBuffer()->SetRegion(secondDestination);
        second = ProductRelocateOrRemap(fixture.collector, fixture.from, fixture.fromPage->generation_id());
        AllocBuffer::GetOrCreateAllocBuffer()->ClearRegion();
    });
    JoinGuard loserGuard(loser);
    while (LockedWaiterWitness::Hits() == 0) {
        std::this_thread::yield();
    }
    AdmissionBarrier::Release();
    winner.join();
    loser.join();
    setAdmission(nullptr);
    setWaiter(nullptr);

    const MAddress firstTop = fixture.destination->GetRegionAllocPtr();
    const MAddress secondTop = secondDestination->GetRegionAllocPtr();
    std::fprintf(stderr,
                 "LAZY_RELOCATION_TARGET_ASSERTION_REACHED case=concurrent-loser waiter_hits=%u relocation=%zu first_top=%#zx second_top=%#zx\n",
                 LockedWaiterWitness::Hits(), AllocBuffer::RelocationAllocationCallsForTest(),
                 static_cast<size_t>(firstTop), static_cast<size_t>(secondTop));
    GC_EXPECT_TRUE(first != nullptr && first == second);
    GC_EXPECT_EQ(AllocBuffer::OrdinaryAllocationCallsForTest() +
                     AllocBuffer::RelocationAllocationCallsForTest(),
                 1u);
    GC_EXPECT_EQ(firstTop, fixture.destination->GetRegionStart() + fixture.objectSize);
    GC_EXPECT_EQ(secondTop, secondDestination->GetRegionStart());
    GC_EXPECT_EQ(ForwardingTable::FindTo(fixture.fromAddress), reinterpret_cast<MAddress>(first));
}

GC_OTHER_VM_TEST(LazyRelocationDestination, ImmediateFailureSkipsOrdinaryAllocator)
{
    LazyFixture fixture;
    const MAddress before = fixture.destination->GetRegionAllocPtr();
    AllocBuffer::ForceRelocationAllocationFailureForTest(true);
    BaseObject* resolved = RelocationReceiptTestAccess::ForwardImpl(
        fixture.collector, fixture.from, fixture.fromPage);
    const MAddress after = fixture.destination->GetRegionAllocPtr();
    const size_t failedOrdinaryCalls = AllocBuffer::OrdinaryAllocationCallsForTest();
    const size_t failedRelocationCalls = AllocBuffer::RelocationAllocationCallsForTest();
    AllocBuffer::ForceRelocationAllocationFailureForTest(false);

    // Positive control for the zero ordinary-call assertion. The same product
    // AllocBuffer and destination page make the counter move when the ordinary
    // contract is explicitly selected.
    AllocBuffer::ResetAllocationPathCountersForTest();
    fixture.buffer->SetRegion(fixture.destination);
    const MAddress ordinary = fixture.buffer->Allocate(
        fixture.objectSize, AllocType::MOVEABLE_OBJECT);
    const size_t controlOrdinaryCalls = AllocBuffer::OrdinaryAllocationCallsForTest();

    std::fprintf(stderr,
                 "LAZY_RELOCATION_TARGET_ASSERTION_REACHED case=immediate-failure before=%#zx after=%#zx ordinary=%zu relocation=%zu control_ordinary=%zu control_addr=%#zx\n",
                 static_cast<size_t>(before), static_cast<size_t>(after), failedOrdinaryCalls,
                 failedRelocationCalls, controlOrdinaryCalls, static_cast<size_t>(ordinary));
    GC_EXPECT_TRUE(resolved == nullptr);
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_TRUE(fixture.from->GetStateWord().GetStateCode() == ObjectState::NORMAL);
    GC_EXPECT_EQ(failedOrdinaryCalls, 0u);
    GC_EXPECT_EQ(failedRelocationCalls, 1u);
    GC_EXPECT_EQ(controlOrdinaryCalls, 1u);
    GC_EXPECT_EQ(ordinary, before);
}
