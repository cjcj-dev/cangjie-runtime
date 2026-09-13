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
#include "gc_unittest.hpp"

namespace MapleRuntime {
struct UncommitterTestAccess {
    static void ResetCancel()
    {
        Uncommitter& worker = Heap::GetHeap().GetAllocator().GetUncommitter();
        worker.canceled.store(false);
        worker.stopped.store(false);
    }
    static bool Activate(Uncommitter& worker) { return worker.Activate(); }
    static size_t Budget(Uncommitter& worker) { return worker.toUncommit; }
    static size_t Uncommit(Uncommitter& worker) { return worker.Uncommit(); }
    static void Register(Uncommitter& worker, size_t size) { worker.RegisterUncommit(size); }
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
    Uncommitter worker(Heap::GetHeap().GetAllocator());
    worker.Start();
    worker.Stop();
    worker.Start();
    worker.Stop();
    GC_EXPECT_FALSE(UncommitterTestAccess::Wait(worker, TimeUtil::NanoSeconds()));
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
    GC_EXPECT_FALSE(Uncommitter::ShouldStopUncommit());
    Uncommitter::CancelCycle();
    GC_EXPECT_TRUE(Uncommitter::ShouldStopUncommit());
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_FALSE(Uncommitter::ShouldStopUncommit());
}

GC_TEST(Uncommitter, PartialPrefixIsRetainedNotRounded)
{
    const size_t requested = 4 * 4096;
    const size_t prefix = 4096;
    GC_EXPECT_TRUE(Uncommitter::ShouldRetryPartial(requested, prefix));
    GC_EXPECT_FALSE(Uncommitter::ShouldRetryPartial(requested, requested));
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

static size_t ProbeProductUncommit(bool cancelFirst, bool honorCancel)
{
    BindUncommitWorkerThread();
    const size_t n = 8;
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
    RegionManager rm;
    FreeRegionManager frm(rm);
    frm.Initialize(n);
    std::fprintf(stderr, "DETAIL probe tree ready\n");
    std::fflush(stderr);
    (void)RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::FREE_UNITS);
    GC_EXPECT_TRUE(frm.releasedUnitTree.MergeInsert(0, 1, false));
    std::fprintf(stderr, "DETAIL probe releasedCount=%u\n", frm.GetReleasedUnitCount());
    std::fflush(stderr);
    UncommitterTestAccess::ResetCancel();
    if (cancelFirst) {
        Uncommitter::CancelCycle();
    }
    const size_t backendReleased =
        frm.UncommitIdleUnits(RegionInfo::UNIT_SIZE, static_cast<uint64_t>(-1), honorCancel);
    std::fprintf(stderr,
                 "DETAIL backendReleased=%zu honorCancel=%d cancelFirst=%d unit=%zu\n",
                 backendReleased, honorCancel ? 1 : 0, cancelFirst ? 1 : 0, RegionInfo::UNIT_SIZE);
    std::fflush(stderr);
    MemMap::DestroyMemMap(map);
    return backendReleased;
}

GC_OTHER_VM_TEST(Uncommitter, UncommitIdleUnitsReleasesPhysical)
{
    const size_t backendReleased = ProbeProductUncommit(false, true);
    GC_EXPECT_TRUE(backendReleased > 0);
}

GC_OTHER_VM_TEST(Uncommitter, CancelDelaysActivation)
{
    UncommitterTestAccess::ResetCancel();
    Uncommitter::CancelCycle();
    Uncommitter& worker = Heap::GetHeap().GetAllocator().GetUncommitter();
    GC_EXPECT_FALSE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_TRUE(Uncommitter::ShouldStopUncommit());
}

GC_OTHER_VM_TEST(Uncommitter, PeriodicUncommitStopsAfterCancel)
{
    const size_t backendReleased = ProbeProductUncommit(true, true);
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
