// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_GC_UNIT_TESTS)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "gc_unittest.hpp"
#include "Heap/Allocator/MemMap.h"
#include "Heap/Allocator/RegionManager.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr auto kHangLimit = std::chrono::seconds(30);

class OneUnitStallFixture {
private:
    struct MapOwner {
        ~MapOwner() { MemMap::DestroyMemMap(value); }
        MemMap* value{ nullptr };
    } map;
    RegionInfo* capacity{ nullptr };

public:
    OneUnitStallFixture()
    {
        constexpr size_t units = 1;
        const size_t metadataSize = RegionManager::GetMetadataSize(units);
        map.value = MemMap::MapMemory(metadataSize + RegionInfo::UNIT_SIZE, metadataSize);
        HeapParam heapParam {};
        heapParam.regionSize = RegionInfo::UNIT_SIZE / 1024;
        heapParam.exemptionThreshold = 0.8;
        manager.Initialize(units, reinterpret_cast<uintptr_t>(map.value->GetBaseAddr()), *map.value, heapParam, 0.5);
        capacity = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS, false, false);
    }

    void PublishCapacity()
    {
        RegionInfo* region = capacity;
        capacity = nullptr;
        manager.ReclaimRegion(region);
    }

    RegionManager manager;
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
    const size_t units = manager.StallAllocation(RegionInfo::UNIT_SIZE);
    claimed.store(units, std::memory_order_release);
    if (units != 0) {
        manager.FinishStalledAllocation(units);
    }
    ThreadLocal::SetMutator(nullptr);
}

} // namespace

namespace {

struct TwoWaiterResult {
    bool beforeWaveReady;
    bool saferegionOk;
    size_t saferegionChecks;
    size_t gcCalls;
    size_t claimedUnits;
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
    GC_EXPECT_EQ(result.claimedUnits, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.satisfied, static_cast<size_t>(1));
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

#endif // MRT_GC_UNIT_TESTS
