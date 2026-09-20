// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zMarkStack.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include <cstring>
#include "zunittest.hpp"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "CjScheduler.h"
extern "C" int CJ_ScheduleManagerInit();
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr auto kHangLimit = std::chrono::seconds(30);

// The production mutator/TLS callbacks are installed by Concurrency::Init.
class StallTestRuntime final : public Runtime {
public:
    StallTestRuntime()
    {
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        const ConcurrencyParam params = { 1024, 64, 1 };
        concurrency.Init(params);
    }
    ~StallTestRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
private:
    MutatorManager manager;
    Concurrency concurrency;
};

class OneUnitStallFixture {
private:
    StallTestRuntime runtime;
    ZPage* capacity{ nullptr };

public:
    // The RegionManager (mapped caches keep entries in heap memory) must be
    // destroyed before the mapping: declare it last.
    std::unique_ptr<ZTestRegionHeap> heap;
    RegionManager manager;

    OneUnitStallFixture()
    {
        // ZInitialize: allocator sampling starts only after statistics initialization.
        ZStat::Initialize();
        constexpr size_t units = 1;
        HeapParam heapParam {};
        heapParam.regionSize = ZGranuleSize / 1024;
        heapParam.exemptionThreshold = 0.8;
        heap.reset(new ZTestRegionHeap(units, manager, heapParam, 0.5));
        BindFixturePageTable(manager, units);
        capacity = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, false);
    }

    ~OneUnitStallFixture()
    {
        Heap::bind_test_page_allocator(nullptr);
    }

    void PublishCapacity()
    {
        ZPage* region = capacity;
        capacity = nullptr;
        manager.ReclaimRegion(region);
    }
};
} // namespace

GC_OTHER_VM_TEST(AllocationStall, OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters)
{
    OneUnitStallFixture fixture;
    fixture.PublishCapacity();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* first = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, true, PageAge::eden, flags);
    ZPage* second = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, true, PageAge::eden, flags);
    const size_t count = (first != nullptr) + (second != nullptr);
    std::fprintf(stderr, "ALLOCATION_CAPACITY_TARGET count=%zu first=%p second=%p\n", count, first, second);
    GC_EXPECT_EQ(count, size_t{1});
}
GC_OTHER_VM_TEST(AllocationStall, OrdinaryAllocationCannotTakeSatisfiedPage)
{
    OneUnitStallFixture fixture;
    fixture.PublishCapacity();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPageAllocation request(ZPageSizeSmall, static_cast<uint8_t>(ZPageType::small), false, true, flags);
    GC_EXPECT_TRUE(fixture.manager.ClaimAllocationLocked(request));
    ZPage* competing = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, true, PageAge::eden, flags);
    std::fprintf(stderr, "ALLOCATION_RESERVATION_TARGET bytes=%zu competing=%p\n", request.Memory().size, competing);
    GC_EXPECT_EQ(request.Memory().size, ZPageSizeSmall);
    GC_EXPECT_TRUE(competing == nullptr);
    fixture.manager.ReturnPageMemory(request.Memory());
}
GC_OTHER_VM_TEST(AllocationStall, WaiterBlocksInSaferegion)
{
    OneUnitStallFixture fixture;
    ZPageAllocation request(ZPageSizeSmall, static_cast<uint8_t>(ZPageType::small), false, true);
    Mutator mutator;
    std::atomic<bool> ready{false}, completed{false};
    bool result = false;
    std::thread waiter([&] {
        mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
        ThreadLocal::SetMutator(&mutator);
        ready.store(true, std::memory_order_release);
        result = fixture.manager.StallAllocation(request, false);
        completed.store(true, std::memory_order_release);
        ThreadLocal::SetMutator(nullptr);
    });
    const auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while ((!ready.load(std::memory_order_acquire) || !mutator.InSaferegion()) &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    const bool safe = ready.load() && mutator.InSaferegion() && !completed.load();
    request.Satisfy(true);
    waiter.join();
    std::fprintf(stderr, "ALLOCATION_WAIT_TARGET safe=%d result=%d completed=%d\n", safe, result, completed.load());
    GC_EXPECT_TRUE(safe);
    GC_EXPECT_TRUE(result && completed.load());
}
GC_TEST(AllocationStall, CompletedWaveDoesNotFailLateWaiter)
{
    std::mutex owner;
    AllocationStallQueue queue(owner);
    ZPageAllocation first(ZPageSizeSmall, 0, false, true), late(ZPageSizeSmall, 0, false, true);
    queue.EnqueueLocked(first);
    const uint64_t boundary = queue.CaptureWaveBoundary();
    queue.EnqueueLocked(late);
    GC_EXPECT_TRUE(queue.CompleteWave(boundary));
    GC_EXPECT_FALSE(first.Wait());
    size_t served = queue.SatisfyAvailable([&](ZPageAllocation& request) { return &request == &late; });
    std::fprintf(stderr, "ALLOCATION_LATE_TARGET served=%zu\n", served);
    GC_EXPECT_EQ(served, size_t{1});
    GC_EXPECT_TRUE(late.Wait());
}
GC_TEST(AllocationStall, DequeueBeforeNotifyKeepsOneTerminalPerWaiter)
{
    std::mutex owner;
    AllocationStallQueue queue(owner);
    ZPageAllocation first(ZPageSizeSmall, 0, false, true), second(ZPageSizeSmall, 0, false, true);
    queue.EnqueueLocked(first);
    queue.EnqueueLocked(second);
    const uint64_t boundary = queue.CaptureWaveBoundary();
    size_t served = queue.SatisfyAvailable([&](ZPageAllocation& request) { return &request == &first; });
    GC_EXPECT_EQ(served, size_t{1});
    GC_EXPECT_FALSE(queue.CompleteWave(boundary));
    GC_EXPECT_TRUE(first.Wait());
    GC_EXPECT_FALSE(second.Wait());
    GC_EXPECT_FALSE(queue.IsStalling());
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uint64_t> stallMarkSequence{0};
std::atomic<bool> stallReleaseMark{false};
std::atomic<bool> stallWindowTimeout{false};

void ObserveStallMark(const std::vector<BaseObject*>* objects)
{
    auto& heap = Heap::GetHeap();
    const auto snapshot = heap.GetCycleSnapshot(ZGenerationId::old);
    if (objects == nullptr || snapshot.phase != ZGenerationPhase::Mark) { return; }
    uint64_t empty = 0;
    if (!stallMarkSequence.compare_exchange_strong(empty, snapshot.sequence)) { return; }
    const auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!stallReleaseMark.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stallWindowTimeout = !stallReleaseMark.load();
}
}

// zPageAllocator.cpp:2320-2362: a request arriving after old mark-start
// cannot be failed by that collection; the remaining queue drives another GC.
// Both waiters enter via Heap::alloc_page, not by constructing queue requests.
GC_OTHER_VM_TEST(AllocationStall, ProductLateWaiterRequiresNextCollection)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(void*));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    U64 root;
    {
        ScopedObjectAccess access;
        root = heap.RegisterExportRoot(MObject::NewPinnedObject(type, 2 * sizeof(void*)));
    }
    stallMarkSequence = 0;
    stallReleaseMark = false;
    stallWindowTimeout = false;
    SetMarkClosureObserverForTest(ObserveStallMark);
    const size_t bytes = heap.GetMaxCapacity(); // the rooted object excludes a whole-heap claim
    ZPage* results[2]{};
    uint64_t completedSequence[2]{};
    std::atomic<bool> done[2]{};
    auto allocate = [&](size_t index) {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            results[index] = Heap::alloc_page(bytes, ZPageType::large);
            completedSequence[index] = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
        }
        done[index].store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    };
    std::thread first(allocate, 0);
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (stallMarkSequence.load() == 0 && !done[0].load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const uint64_t firstMark = stallMarkSequence.load();
    std::thread late(allocate, 1);
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (heap.page_allocator().PendingStalledAllocations() != 2 && !done[1].load() &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    const size_t queued = heap.page_allocator().PendingStalledAllocations();
    stallReleaseMark.store(true, std::memory_order_release);
    first.join();
    late.join();
    SetMarkClosureObserverForTest(nullptr);
    const bool retained = heap.GetExportObject(root) != nullptr;
    std::fprintf(stderr, "STALL_PRODUCT_LATE_TARGET queued=%zu first_mark=%llu first_done=%llu late_done=%llu "
                 "first_null=%d late_null=%d retained=%d timeout=%d\n", queued,
                 (unsigned long long)firstMark, (unsigned long long)completedSequence[0],
                 (unsigned long long)completedSequence[1], results[0] == nullptr, results[1] == nullptr,
                 retained, stallWindowTimeout.load());
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_EQ(queued, size_t{2});
    GC_EXPECT_TRUE(firstMark != 0 && !stallWindowTimeout.load());
    GC_EXPECT_TRUE(results[0] == nullptr && results[1] == nullptr && retained);
    GC_EXPECT_TRUE(completedSequence[1] > firstMark);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif
