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
        heapParam.regionSize = ZPage::UNIT_SIZE / 1024;
        heapParam.exemptionThreshold = 0.8;
        heap.reset(new ZTestRegionHeap(units, manager, heapParam, 0.5));
        BindFixturePageTable(manager, units);
        capacity = manager.TakeRegion(1, ZPageType::small, false, false);
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
    ZPage* region = manager.TakeRegion(1, ZPageType::small);
    PublishAllocatedPage(region);
    claimed.store(region == nullptr ? 0 : region->GetUnitCount(), std::memory_order_release);
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

GC_OTHER_VM_TEST(AllocationStall, OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters)
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
GC_OTHER_VM_TEST(AllocationStall, OrdinaryAllocationCannotTakeSatisfiedPage)
{
    OneUnitStallFixture fixture;
    ZPage* competing = nullptr;
    bool supplied = false;
    fixture.manager.SetAllocationStallTestHooks(
        [](RegionManager&) {},
        [&](RegionManager& manager) {
            fixture.PublishCapacity();
            supplied = true;
            competing = manager.TakeRegion(1, ZPageType::small, false, false);
        },
        {});
    std::atomic<size_t> allocated{ 0 };
    std::thread waiter([&] { RunWaiter(fixture.manager, allocated); });
    waiter.join();
    GC_EXPECT_TRUE(supplied);
    GC_EXPECT_TRUE(competing == nullptr);
    GC_EXPECT_EQ(allocated.load(std::memory_order_acquire), static_cast<size_t>(1));
}

GC_OTHER_VM_TEST(AllocationStall, WaiterBlocksInSaferegion)
{
    const TwoWaiterResult result = RunTwoWaiterCapacityScenario();
    GC_EXPECT_TRUE(result.beforeWaveReady);
    GC_EXPECT_TRUE(result.saferegionOk);
    GC_EXPECT_TRUE(result.saferegionChecks >= 1);
}

GC_OTHER_VM_TEST(AllocationStall, DequeueBeforeNotifyKeepsOneTerminalPerWaiter)
{
    const TwoWaiterResult result = RunTwoWaiterCapacityScenario();
    GC_EXPECT_TRUE(result.beforeWaveReady);
    GC_EXPECT_EQ(result.enqueued, static_cast<size_t>(2));
    GC_EXPECT_EQ(result.dequeued, static_cast<size_t>(2));
    GC_EXPECT_EQ(result.failed, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.pending, static_cast<size_t>(0));
}

GC_OTHER_VM_TEST(AllocationStall, CompletedWaveDoesNotFailLateWaiter)
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

#endif // MRT_GC_UNIT_TESTS || MRT_TESTABLE_INTERNALS
