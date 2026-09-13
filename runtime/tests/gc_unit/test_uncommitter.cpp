// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <limits>
#include <future>
#include <chrono>

#include "Heap/Allocator/CartesianTree.h"
#define private public
#include "Heap/Allocator/FreeRegionManager.h"
#include "Heap/Allocator/RegionManager.h"
#undef private
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/MemMap.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Uncommitter.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "Heap/Heap.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "CjScheduler.h"

extern "C" int CJ_ScheduleManagerInit();
#include "gc_unittest.hpp"

namespace MapleRuntime {
struct UncommitterTestAccess {
    static void ResetCancel()
    {
        Uncommitter& worker = Heap::GetHeap().GetAllocator().GetUncommitter();
        auto& regions = static_cast<RegionSpace&>(worker.partition).GetRegionManager();
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        worker.canceled = false;
        worker.stopped.store(false);
    }
    static bool Activate(Uncommitter& worker) { return worker.Activate(); }
    static size_t Budget(Uncommitter& worker) { return worker.toUncommit; }
    static size_t Uncommit(Uncommitter& worker) { return worker.Uncommit(); }
    static void Cancel()
    {
        auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        Uncommitter::CancelCycleLocked();
    }
    static bool Canceled()
    {
        auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        return Heap::GetHeap().GetAllocator().GetUncommitter().canceled;
    }
    static void PrepareChunk(Uncommitter& worker)
    {
        worker.cycleStart = TimeUtil::NanoSeconds() + Uncommitter::DelayNs();
        worker.toUncommit = RegionInfo::UNIT_SIZE;
    }
    static bool Wait(Uncommitter& worker, uint64_t deadline) { return worker.WaitUntil(deadline); }
};
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(Uncommitter, ParseDelayDefaultAndOff)
{
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs(nullptr), Uncommitter::kDefaultDelayNs);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("0"), 0ULL);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("0s"), 0ULL);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("20s"), 20ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("20"), 20ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("300s"), 300ULL * SECOND_TO_NANO_SECOND);
}

GC_TEST(Uncommitter, MinCapacityIsLivePlusYoungReserve)
{
    GC_EXPECT_EQ(Uncommitter::MinCapacity(10 * MB, 32 * MB), 42 * MB);
    GC_EXPECT_EQ(Uncommitter::MinCapacity(0, 32 * MB), 32 * MB);
}

// Port of gc/z/TestNoUncommit.java: a partition at its capacity floor
// cannot supply any uncommit budget.
GC_OTHER_VM_TEST(Uncommitter, TestNoUncommitAtCapacityFloor)
{
    Uncommitter worker(Heap::GetHeap().GetAllocator());
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_EQ(UncommitterTestAccess::Budget(worker), 0U);
    GC_EXPECT_EQ(UncommitterTestAccess::Uncommit(worker), 0U);
}

// ZUncommitter::terminate must wake a worker even during a long delay.
GC_OTHER_VM_TEST(Uncommitter, StopWakesDelayedWorker)
{
    Uncommitter worker(Heap::GetHeap().GetAllocator());
    std::promise<void> entered;
    auto result = std::async(std::launch::async, [&] {
        entered.set_value();
        return UncommitterTestAccess::Wait(worker, TimeUtil::NanoSeconds() + 3600ULL * SECOND_TO_NANO_SECOND);
    });
    entered.get_future().wait();
    worker.Stop();
    GC_EXPECT_TRUE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    GC_EXPECT_FALSE(result.get());
}

GC_OTHER_VM_TEST(Uncommitter, StartStopRestartPartitionWorker)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    Heap::GetHeap().GetAllocator().GetUncommitter().Stop();
    Uncommitter worker(Heap::GetHeap().GetAllocator());
    worker.Start();
    worker.Stop();
    worker.Start();
    worker.Stop();
    GC_EXPECT_FALSE(UncommitterTestAccess::Wait(worker, TimeUtil::NanoSeconds()));
}

// Native counterpart of ZUncommitter's suspendible-thread membership:
// the real worker is registered while waiting and removed before Stop returns.
GC_OTHER_VM_TEST(Uncommitter, PartitionWorkerParticipatesInSafepoints)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    auto& worker = Heap::GetHeap().GetAllocator().GetUncommitter();
    worker.Stop();
    auto& manager = MutatorManager::Instance();
    const size_t before = manager.RuntimeMutatorRegistrySizeForTest();
    worker.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (manager.RuntimeMutatorRegistrySizeForTest() == before &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const size_t registered = manager.RuntimeMutatorRegistrySizeForTest();
    worker.Stop();
    GC_EXPECT_EQ(registered, before + 1);
    GC_EXPECT_EQ(manager.RuntimeMutatorRegistrySizeForTest(), before);
}

GC_TEST(Uncommitter, ChunkLimitAtLeastPageAndAtMost256M)
{
    size_t oneG = 1024 * MB;
    size_t chunk = Uncommitter::ChunkLimit(oneG);
    GC_EXPECT_TRUE(chunk >= 4096);
    GC_EXPECT_TRUE(chunk <= Uncommitter::kMaxUncommitChunk);
    GC_EXPECT_EQ(Uncommitter::ChunkLimit(32 * GB), Uncommitter::kMaxUncommitChunk);
}

GC_TEST(Uncommitter, IdleTreeHonorsVirtualClockAndChunkOwnership)
{
    CartesianTree tree;
    tree.Init(32);
    GC_EXPECT_TRUE(tree.MergeInsert(4, 8, false));

    CartesianTree::Index idx = 0;
    CartesianTree::Count count = 0;
    GC_EXPECT_FALSE(tree.TakeIdleUnits(0, 8, idx, count));
    GC_EXPECT_TRUE(tree.TakeIdleUnits(std::numeric_limits<uint64_t>::max(), 3, idx, count));
    GC_EXPECT_EQ(idx, 4U);
    GC_EXPECT_EQ(count, 3U);
    GC_EXPECT_EQ(tree.GetTotalCount(), 5U);
    tree.Fini();
}

GC_TEST(Uncommitter, CycleCancelStopsUncommit)
{
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_FALSE(UncommitterTestAccess::Canceled());
    UncommitterTestAccess::Cancel();
    GC_EXPECT_TRUE(UncommitterTestAccess::Canceled());
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_FALSE(UncommitterTestAccess::Canceled());
}

// ZPhysicalMemoryManager's partial completion feeds ZUncommitter::register_uncommit.
GC_TEST(Uncommitter, PartialPrefixIsRetainedNotRounded)
{
    struct PartialBackend final : MemMapBackend {
        void* Reserve(void*, size_t, unsigned int, const char*, bool) override
        { return reinterpret_cast<void*>(0x10000000); }
        size_t Commit(void*, size_t size, int, uint32_t, bool) override { return size; }
        bool Protect(void*, size_t, int) override { return true; }
        size_t Release(void*, size_t, uint32_t) override { return 4096; }
        bool Unreserve(void*, size_t) override { return true; }
    } backend;
    const size_t requested = 4 * 4096;
    MemMap* map = MemMap::TryMapMemory(requested, requested, MemMap::DEFAULT_OPTIONS,
        AddressSpaceBudget::Seal(1024 * 1024), NumaTopology::Seal({0}), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const size_t before = map->GetCommittedSize();
    const size_t completed = map->ReleaseMemoryDeferred(map->GetBaseAddr(), requested);
    GC_EXPECT_EQ(completed, 4096U);
    GC_EXPECT_EQ(map->GetCommittedSize(), before);
    GC_EXPECT_EQ(map->PublishMemoryRelease(map->GetBaseAddr(), completed), completed);
    GC_EXPECT_EQ(map->GetCommittedSize(), before - completed);
    GC_EXPECT_EQ(map->PublishMemoryRelease(map->GetBaseAddr(), completed), 0U);
    MemMap::DestroyMemMap(map);
}

GC_TEST(Uncommitter, AllocationTakeBumpsIdleClock)
{
    CartesianTree tree;
    tree.Init(16);
    GC_EXPECT_TRUE(tree.MergeInsert(0, 8, false));
    uint64_t before = tree.GetLastUsedNs();
    CartesianTree::Index idx = 0;
    GC_EXPECT_TRUE(tree.TakeUnits(2, idx, false));
    GC_EXPECT_TRUE(tree.GetLastUsedNs() >= before);
    CartesianTree::Count count = 0;
    GC_EXPECT_FALSE(tree.TakeIdleUnits(before, 2, idx, count));
    tree.Fini();
}

static void BindUncommitWorkerThread()
{
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
}

static size_t ProbeProductUncommit(bool cancelFirst)
{
    BindUncommitWorkerThread();
    const size_t n = 64 * MB / RegionInfo::UNIT_SIZE;
    const size_t meta = RegionManager::GetMetadataSize(n);
    const size_t heapBytes = n * RegionInfo::UNIT_SIZE;
    const size_t total = meta + heapBytes;
    std::fprintf(stderr, "DETAIL probe n=%zu meta=%zu total=%zu\n", n, meta, total);
    std::fflush(stderr);
    MemMap* map = MemMap::MapMemory(total, total);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t heapStart = reinterpret_cast<uintptr_t>(map->GetBaseAddr()) + meta;
    std::fprintf(stderr, "DETAIL probe mapped base=%p heapStart=%#zx\n", map->GetBaseAddr(), heapStart);
    std::fflush(stderr);
    RegionInfo::Initialize(n, heapStart, map);
    std::fprintf(stderr, "DETAIL probe initialized\n");
    std::fflush(stderr);
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& rm = space.GetRegionManager();
    FreeRegionManager& frm = rm.freeRegionManager;
    frm.Initialize(n);
    std::fprintf(stderr, "DETAIL probe tree ready\n");
    std::fflush(stderr);
    (void)RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::FREE_UNITS);
    GC_EXPECT_TRUE(frm.releasedUnitTree.MergeInsert(0, 1, false));
    std::fprintf(stderr, "DETAIL probe releasedCount=%u\n", frm.GetReleasedUnitCount());
    std::fflush(stderr);
    UncommitterTestAccess::ResetCancel();
    if (cancelFirst) {
        UncommitterTestAccess::Cancel();
    }
    Uncommitter& worker = space.GetUncommitter();
    UncommitterTestAccess::PrepareChunk(worker);
    const size_t backendReleased = UncommitterTestAccess::Uncommit(worker);
    std::fprintf(stderr,
                 "DETAIL backendReleased=%zu cancelFirst=%d unit=%zu\n",
                 backendReleased, cancelFirst ? 1 : 0, RegionInfo::UNIT_SIZE);
    std::fflush(stderr);
    MemMap::DestroyMemMap(map);
    return backendReleased;
}

GC_OTHER_VM_TEST(Uncommitter, UncommitIdleUnitsReleasesPhysical)
{
    const size_t backendReleased = ProbeProductUncommit(false);
    GC_EXPECT_TRUE(backendReleased > 0);
}

// Ports of gc/z/TestUncommit.java and TestNoUncommit.java. These tests start
// the allocator-owned product thread and observe the physical backing owner's
// capacity, rather than a modeled counter. Each runs in a fresh process so the
// process-wide delay setting and RegionInfo reservation belong to this test.
static void ExercisePartitionWorker(bool enabled)
{
    GC_EXPECT_EQ(setenv("cjUncommitDelay", enabled ? "10ms" : "0", 1), 0);
    BindUncommitWorkerThread();
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& regions = space.GetRegionManager();
    space.GetUncommitter().Stop();
    const size_t n = 64 * MB / RegionInfo::UNIT_SIZE;
    RegionInfo* region = regions.TakeRegion(n, RegionInfo::UnitRole::LARGE_SIZED_UNITS, true, false);
    GC_EXPECT_TRUE(region != nullptr);
    regions.ReturnPageMemory(PageMemory{region->GetUnitIdx(), n, 0, true});
    regions.ReleaseGarbageRegions(0);
    const size_t before = regions.GetCommittedCapacity();
    const uint64_t start = TimeUtil::NanoSeconds();
    Uncommitter& worker = space.GetUncommitter();
    worker.Start();
    const auto deadline = std::chrono::steady_clock::now() +
        (enabled ? std::chrono::seconds(2) : std::chrono::milliseconds(50));
    while (regions.GetCommittedCapacity() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const uint64_t observed = TimeUtil::NanoSeconds();
    worker.Stop();
    const size_t after = regions.GetCommittedCapacity();
    std::fprintf(stderr, "DETAIL partition-worker enabled=%d committed-before=%zu committed-after=%zu elapsed=%llu\n",
                 enabled, before, after, static_cast<unsigned long long>(observed - start));
    GC_EXPECT_TRUE(after >= Uncommitter::MinCapacity(regions.pageAllocatorUsed, 32 * MB));
    if (enabled) {
        GC_EXPECT_TRUE(after < before);
        GC_EXPECT_TRUE(observed - start >= Uncommitter::DelayNs());
        GC_EXPECT_TRUE(after >= 32 * MB);
    } else {
        GC_EXPECT_EQ(after, before);
    }
}

GC_OTHER_VM_TEST(Uncommitter, TestUncommitIndependentPartitionThread)
{
    ExercisePartitionWorker(true);
}

GC_OTHER_VM_TEST(Uncommitter, TestNoUncommitDisabledPartitionThread)
{
    ExercisePartitionWorker(false);
}

GC_OTHER_VM_TEST(Uncommitter, CancelDelaysActivation)
{
    UncommitterTestAccess::ResetCancel();
    UncommitterTestAccess::Cancel();
    Uncommitter& worker = Heap::GetHeap().GetAllocator().GetUncommitter();
    GC_EXPECT_FALSE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_TRUE(UncommitterTestAccess::Canceled());
}

GC_OTHER_VM_TEST(Uncommitter, PeriodicUncommitStopsAfterCancel)
{
    const size_t backendReleased = ProbeProductUncommit(true);
    GC_EXPECT_EQ(backendReleased, 0U);
}

GC_TEST(Uncommitter, LiveForwardingBlocksReleasedCache)
{
    if (ForwardingTable::Ready()) {
        std::fprintf(stderr,
            "SKIP_ALREADY_OWNED reason=granule_map_already_bound_by_this_process test=Uncommitter.LiveForwardingBlocksReleasedCache\n");
        return;
    }
    BindUncommitWorkerThread();
    const size_t n = 8;
    const size_t meta = RegionManager::GetMetadataSize(n);
    const size_t heapBytes = n * RegionInfo::UNIT_SIZE;
    const size_t total = meta + heapBytes;
    MemMap* map = MemMap::MapMemory(total, total);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t heapStart = reinterpret_cast<uintptr_t>(map->GetBaseAddr()) + meta;
    RegionInfo::Initialize(n, heapStart, map);
    RegionInfo* region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::FREE_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    ForwardingTable::Initialize(static_cast<MAddress>(heapStart), heapBytes, RegionInfo::UNIT_SIZE);
    RegionManager rm;
    FreeRegionManager frm(rm);
    frm.Initialize(n);

    frm.AddReleaseUnits(0, 1);
    GC_EXPECT_EQ(frm.GetReleasedUnitCount(), 1U);
    CartesianTree::Index idx = 0;
    GC_EXPECT_TRUE(frm.releasedUnitTree.TakeUnits(1, idx, false));

    if (!ForwardingTable::InsertProvisional(region->GetRegionStart(), region->GetRegionSize(), region)) {
        GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
            region->GetRegionStart(), region->GetRegionSize()));
        GC_EXPECT_TRUE(ForwardingTable::InsertProvisional(
            region->GetRegionStart(), region->GetRegionSize(), region));
    }
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(region->GetRegionStart()) != nullptr);
    GC_EXPECT_FALSE(FreeRegionManager::ExtentReadyForReleasedCache(region));

    frm.AddReleaseUnits(0, 1);
    std::fprintf(stderr, "DETAIL liveFwd released=%u quarantine=%d\n",
                 frm.GetReleasedUnitCount(), frm.HasDetachQuarantine() ? 1 : 0);
    std::fflush(stderr);
    GC_EXPECT_EQ(frm.GetReleasedUnitCount(), 1U);
    GC_EXPECT_FALSE(frm.HasDetachQuarantine());

    CartesianTree::Index takeIdx = 0;
    GC_EXPECT_TRUE(frm.releasedUnitTree.TakeUnits(1, takeIdx, false));
    GC_EXPECT_EQ(takeIdx, 0U);
    GC_EXPECT_TRUE(frm.releasedUnitTree.MergeInsert(0, 1, false));

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    ForwardingTable::ReclaimRetired("Uncommitter.LiveForwardingTableBlocksReleasedCache.cleanup");
    ForwardingTable::Remove(region->GetRegionStart(), region->GetRegionSize());
    MemMap::DestroyMemMap(map);
}

GC_TEST(Uncommitter, LiveForwardingRefCountKeepsReleasedAllocatable)
{
    BindUncommitWorkerThread();
    const size_t n = 8;
    const size_t meta = RegionManager::GetMetadataSize(n);
    const size_t heapBytes = n * RegionInfo::UNIT_SIZE;
    const size_t total = meta + heapBytes;
    MemMap* map = MemMap::MapMemory(total, total);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t heapStart = reinterpret_cast<uintptr_t>(map->GetBaseAddr()) + meta;
    RegionInfo::Initialize(n, heapStart, map);
    RegionInfo* region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::FREE_UNITS);
    GC_EXPECT_TRUE(region != nullptr);
    RegionManager rm;
    FreeRegionManager frm(rm);
    frm.Initialize(n);
    frm.AddReleaseUnits(0, 1);
    ZForwarding* owner = ZForwarding::alloc(1, region->GetRegionStart(), region->GetRegionStart(),
                                          region->GetRegionSize(), region, region->GetRegionLifeId());
    GC_EXPECT_TRUE(owner != nullptr);
    owner->retain_owner();
    region->metadata.fwdOwner.store(owner, std::memory_order_release);
    GC_EXPECT_TRUE(region->RetainForwarding());
    GC_EXPECT_TRUE(region->ForwardingRefCount() != 0);
    GC_EXPECT_FALSE(FreeRegionManager::ExtentReadyForReleasedCache(region));
    GC_EXPECT_EQ(frm.GetReleasedUnitCount(), 1U);
    GC_EXPECT_FALSE(frm.HasDetachQuarantine());
    std::fprintf(stderr, "DETAIL refCount released=%u quarantine=%d ref=%d ready=%d\n",
                 frm.GetReleasedUnitCount(), frm.HasDetachQuarantine() ? 1 : 0,
                 region->ForwardingRefCount(),
                 FreeRegionManager::ExtentReadyForReleasedCache(region) ? 1 : 0);
    std::fflush(stderr);
    region->ReleaseForwarding();
    owner->release_page();
    ForwardingTable::ClearPageOwner(region);
    owner->Destroy();
    MemMap::DestroyMemMap(map);
}
