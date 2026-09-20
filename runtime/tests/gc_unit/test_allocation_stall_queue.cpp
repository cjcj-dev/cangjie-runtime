// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS) || defined(MRT_TESTABLE_INTERNALS)

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
    RegionManager& manager;

    OneUnitStallFixture()
        : manager((MapleRuntime::GcUnit::CreateStandaloneHeap(1), Heap::GetHeap().page_allocator()))
    {
        ZStat::Initialize();
        capacity = manager.TakeRegion(ZPageSizeSmall, ZPageType::small, false, false, false);
        PublishAllocatedPage(capacity);
    }


    void PublishCapacity()
    {
        ZPage* region = capacity;
        capacity = nullptr;
        manager.ReclaimRegion(region);
    }
};

struct WaitState {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> saferegionOk{ true };
    std::atomic<size_t> saferegionChecks{ 0 };

    void ObserveWaiter(RegionManager&)
    {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator == nullptr || !mutator->InSaferegion()) {
            saferegionOk.store(false, std::memory_order_release);
        }
        saferegionChecks.fetch_add(1, std::memory_order_relaxed);
        condition.notify_all();
    }

    bool WaitForPending(RegionManager& manager, size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, kHangLimit,
            [&] { return manager.PendingStalledAllocations() >= count; });
    }
};

void RunWaiter(RegionManager& manager, std::atomic<size_t>& claimed)
{
    Mutator mutator;
    mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
    ThreadLocal::SetMutator(&mutator);
    ZPage* region = manager.TakeRegion(ZPageSizeSmall, ZPageType::small, false, true, false);
    PublishAllocatedPage(region);
    claimed.store(region == nullptr ? 0 : (region->GetRegionSize() / ZGranuleSize), std::memory_order_release);
    ThreadLocal::SetMutator(nullptr);
}

} // namespace

namespace {

struct TwoWaiterResult {
    bool beforeWaveReady;
    bool saferegionOk;
    size_t saferegionChecks;
    size_t gcCalls;
    size_t allocatedUnits;
    size_t enqueued;
    size_t dequeued;
    size_t satisfied;
    size_t failed;
    size_t pending;
};

TwoWaiterResult RunTwoWaiterCapacityScenario()
{
    OneUnitStallFixture fixture;
    WaitState waitState;
    std::atomic<bool> beforeWaveReady{ true };
    std::atomic<size_t> gcCalls{ 0 };
    fixture.manager.SetAllocationStallTestHooks(
        [&](RegionManager& manager) {
            if (!waitState.WaitForPending(manager, 2)) {
                beforeWaveReady.store(false, std::memory_order_release);
            }
        },
        [&](RegionManager&) {
            gcCalls.fetch_add(1, std::memory_order_relaxed);
            fixture.PublishCapacity();
        },
        [&](RegionManager& manager) { waitState.ObserveWaiter(manager); });

    std::atomic<size_t> firstClaim{ 0 };
    std::atomic<size_t> secondClaim{ 0 };
    std::thread first([&] { RunWaiter(fixture.manager, firstClaim); });
    std::thread second([&] { RunWaiter(fixture.manager, secondClaim); });
    first.join();
    second.join();

    return {
        beforeWaveReady.load(std::memory_order_acquire),
        waitState.saferegionOk.load(std::memory_order_acquire),
        waitState.saferegionChecks.load(std::memory_order_acquire),
        gcCalls.load(std::memory_order_acquire),
        firstClaim.load(std::memory_order_acquire) + secondClaim.load(std::memory_order_acquire),
        fixture.manager.EnqueuedStalledAllocations(),
        fixture.manager.DequeuedStalledAllocations(),
        fixture.manager.SatisfiedStalledAllocations(),
        fixture.manager.FailedStalledAllocations(),
        fixture.manager.PendingStalledAllocations(),
    };
}

} // namespace

GC_COMPONENT_OTHER_VM_TEST(AllocationStall, OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters)
{
    const TwoWaiterResult result = RunTwoWaiterCapacityScenario();
    GC_EXPECT_TRUE(result.beforeWaveReady);
    GC_EXPECT_EQ(result.gcCalls, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.allocatedUnits, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.satisfied, static_cast<size_t>(1));
}

// ZGC zPageAllocator.cpp:1518 / 2167: a satisfied page is already removed
// from the same supply ordinary allocation uses. Do not return it until the
// stalled caller has consumed it.
GC_COMPONENT_OTHER_VM_TEST(AllocationStall, OrdinaryAllocationCannotTakeSatisfiedPage)
{
    OneUnitStallFixture fixture;
    ZPage* competing = nullptr;
    bool supplied = false;
    fixture.manager.SetAllocationStallTestHooks(
        [](RegionManager&) {},
        [&](RegionManager& manager) {
            fixture.PublishCapacity();
            supplied = true;
            competing = manager.TakeRegion((1) * ZGranuleSize, ZPageType::small, false, false);
        },
        {});
    std::atomic<size_t> allocated{ 0 };
    std::thread waiter([&] { RunWaiter(fixture.manager, allocated); });
    waiter.join();
    GC_EXPECT_TRUE(supplied);
    GC_EXPECT_TRUE(competing == nullptr);
    GC_EXPECT_EQ(allocated.load(std::memory_order_acquire), static_cast<size_t>(1));
}

GC_COMPONENT_OTHER_VM_TEST(AllocationStall, WaiterBlocksInSaferegion)
{
    const TwoWaiterResult result = RunTwoWaiterCapacityScenario();
    GC_EXPECT_TRUE(result.beforeWaveReady);
    GC_EXPECT_TRUE(result.saferegionOk);
    GC_EXPECT_TRUE(result.saferegionChecks >= 1);
}

GC_COMPONENT_OTHER_VM_TEST(AllocationStall, DequeueBeforeNotifyKeepsOneTerminalPerWaiter)
{
    const TwoWaiterResult result = RunTwoWaiterCapacityScenario();
    GC_EXPECT_TRUE(result.beforeWaveReady);
    GC_EXPECT_EQ(result.enqueued, static_cast<size_t>(2));
    GC_EXPECT_EQ(result.dequeued, static_cast<size_t>(2));
    GC_EXPECT_EQ(result.failed, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.pending, static_cast<size_t>(0));
}

GC_COMPONENT_OTHER_VM_TEST(AllocationStall, CompletedWaveDoesNotFailLateWaiter)
{
    OneUnitStallFixture fixture;
    WaitState waitState;
    std::mutex phaseMutex;
    std::condition_variable phaseCondition;
    bool firstGcEntered = false;
    std::atomic<bool> lateEnqueued{ true };
    std::atomic<size_t> gcCalls{ 0 };

    fixture.manager.SetAllocationStallTestHooks(
        [](RegionManager&) {},
        [&](RegionManager& manager) {
            const size_t call = gcCalls.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (call == 1) {
                {
                    std::lock_guard<std::mutex> lock(phaseMutex);
                    firstGcEntered = true;
                }
                phaseCondition.notify_all();
                if (!waitState.WaitForPending(manager, 2)) {
                    lateEnqueued.store(false, std::memory_order_release);
                }
                return;
            }
            fixture.PublishCapacity();
        },
        [&](RegionManager& manager) { waitState.ObserveWaiter(manager); });

    std::atomic<size_t> firstClaim{ 0 };
    std::atomic<size_t> lateClaim{ 0 };
    std::thread first([&] { RunWaiter(fixture.manager, firstClaim); });
    {
        std::unique_lock<std::mutex> lock(phaseMutex);
        GC_EXPECT_TRUE(phaseCondition.wait_for(lock, kHangLimit, [&] { return firstGcEntered; }));
    }
    std::thread late([&] { RunWaiter(fixture.manager, lateClaim); });
    first.join();
    late.join();

    GC_EXPECT_TRUE(lateEnqueued.load(std::memory_order_acquire));
    GC_EXPECT_EQ(gcCalls.load(std::memory_order_acquire), static_cast<size_t>(2));
    GC_EXPECT_EQ(firstClaim.load(std::memory_order_acquire), static_cast<size_t>(0));
    GC_EXPECT_EQ(lateClaim.load(std::memory_order_acquire), static_cast<size_t>(1));
    GC_EXPECT_EQ(fixture.manager.PendingStalledAllocations(), static_cast<size_t>(0));
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
    const size_t bytes = heap.GetMaxCapacity();
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

#endif // MRT_GC_UNIT_TESTS || MRT_TESTABLE_INTERNALS
