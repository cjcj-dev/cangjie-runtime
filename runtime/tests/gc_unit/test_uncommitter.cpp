// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <limits>
#include <future>
#include <chrono>
#include <thread>

#include "Heap/Allocator/CartesianTree.h"
#define private public
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zPageAllocator.hpp"
#undef private
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "zunittest.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "CjScheduler.h"

extern "C" int CJ_ScheduleManagerInit();
#include "gc_unittest.hpp"

#include "gc_product_access_test.hpp"

namespace MapleRuntime {
struct UncommitterTestAccess {
    static ZPartition& Partition()
    {
        return *Heap::GetHeap().GetAllocator().GetRegionManager().freeRegionManager.partitions.front();
    }
    static Uncommitter& Current() { return Partition().uncommitter; }
    static void StopAll()
    {
        Heap::GetHeap().GetAllocator().GetRegionManager().freeRegionManager.StopUncommitters();
    }
    static void ResetCancel()
    {
        Uncommitter& worker = UncommitterTestAccess::Current();
        auto& regions = worker.partition.regionManager;
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
        Current().Cancel();
    }
    static bool Canceled()
    {
        auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        return UncommitterTestAccess::Current().canceled;
    }
    static void PrepareChunk(Uncommitter& worker)
    {
        worker.partition.cache.reset_min_size_watermark();
        worker.cycleStart = TimeUtil::NanoSeconds() + Uncommitter::DelayNs();
        worker.toUncommit = ZGranuleSize;
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

// Port of gc/z/TestNoUncommit.java: a partition at its capacity floor
// cannot supply any uncommit budget.
GC_OTHER_VM_TEST(Uncommitter, TestNoUncommitAtCapacityFloor)
{
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZPartition partition(0, Heap::GetHeap().GetAllocator().GetRegionManager());
    Uncommitter& worker = partition.uncommitter;
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_EQ(UncommitterTestAccess::Budget(worker), 0U);
    GC_EXPECT_EQ(UncommitterTestAccess::Uncommit(worker), 0U);
}

// ZUncommitter::terminate must wake a worker even during a long delay.
GC_OTHER_VM_TEST(Uncommitter, StopWakesDelayedWorker)
{
    ZPartition partition(0, Heap::GetHeap().GetAllocator().GetRegionManager());
    Uncommitter& worker = partition.uncommitter;
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

GC_RUNTIME_OTHER_VM_TEST(Uncommitter, StartStopRestartPartitionWorker)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    UncommitterTestAccess::StopAll();
    ZPartition partition(0, Heap::GetHeap().GetAllocator().GetRegionManager());
    Uncommitter& worker = partition.uncommitter;
    worker.Start();
    worker.Stop();
    worker.Start();
    worker.Stop();
    GC_EXPECT_FALSE(UncommitterTestAccess::Wait(worker, TimeUtil::NanoSeconds()));
}

// Native counterpart of ZUncommitter's suspendible-thread membership:
// the real worker is registered while waiting and removed before Stop returns.
#if defined(MRT_TESTABLE_INTERNALS)
GC_RUNTIME_OTHER_VM_TEST(Uncommitter, PartitionWorkerParticipatesInSafepoints)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    auto& worker = UncommitterTestAccess::Current();
    worker.Stop();
    auto& manager = MutatorManager::Instance();
    const size_t before = MutatorManagerTest::RegistrySize(manager);
    worker.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (MutatorManagerTest::RegistrySize(manager) == before &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const size_t registered = MutatorManagerTest::RegistrySize(manager);
    worker.Stop();
    GC_EXPECT_EQ(registered, before + 1);
    GC_EXPECT_EQ(MutatorManagerTest::RegistrySize(manager), before);
}

#endif // MRT_TESTABLE_INTERNALS

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

GC_COMPONENT_OTHER_VM_TEST(Uncommitter, CycleCancelStopsUncommit)
{
    CreateStandaloneHeap(64 * MB / ZGranuleSize);
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_FALSE(UncommitterTestAccess::Canceled());
    UncommitterTestAccess::Cancel();
    GC_EXPECT_TRUE(UncommitterTestAccess::Canceled());
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_FALSE(UncommitterTestAccess::Canceled());
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

// ZPageAllocator::prime (zPageAllocator.cpp:954-997): claim virtual and
// physical memory for the whole capacity, commit, map, and cache it.
struct ProbeHeap {
    size_t units;
    MAddress start;

    explicit ProbeHeap(size_t n) : units(n)
    {
        CreateStandaloneHeap(n);
        start = Heap::GetHeap().page_allocator().GetRegionHeapStart();
    }
};

static void InitializeUncommitCache(FreeRegionManager& frm, ProbeHeap& heap)
{
    const size_t bytes = heap.units * ZGranuleSize;
    (void)bytes; // ProbeHeap initialized the capacity owner and its free cache together.
    // Prime partition 0 with all of its capacity: claim, commit, map, cache.
    const size_t primed = frm.partitions.front()->currentMaxCapacity;
    const ZVirtualMemory vmem = frm.claim_virtual(primed, 0);
    GC_EXPECT_FALSE(vmem.is_null());
    GC_EXPECT_EQ(frm.increase_capacity(0, primed), primed);
    frm.claim_physical(vmem, 0);
    GC_EXPECT_EQ(frm.commit_physical(vmem, 0), primed);
    frm.map_virtual(vmem, 0);
    frm.partitions.front()->cache.insert(vmem);
}

static size_t ProbeProductUncommit(bool cancelFirst)
{
    BindUncommitWorkerThread();
    const size_t n = 64 * MB / ZGranuleSize;
    std::fprintf(stderr, "DETAIL probe n=%zu\n", n);
    std::fflush(stderr);
    ProbeHeap heap(n);
    auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& rm = space.GetRegionManager();
    FreeRegionManager& frm = rm.freeRegionManager;
    InitializeUncommitCache(frm, heap);
    std::fprintf(stderr, "DETAIL probe cache ready committed=%zu cached=%u\n", frm.capacity(), (frm.GetCachedBytes() / ZGranuleSize));
    std::fflush(stderr);
    // ZGC caches virtual memory after withdrawing the page-table entry.
    GC_EXPECT_TRUE(Heap::page(heap.start) == nullptr);
    UncommitterTestAccess::ResetCancel();
    if (cancelFirst) {
        UncommitterTestAccess::Cancel();
    }
    Uncommitter& worker = UncommitterTestAccess::Current();
    UncommitterTestAccess::PrepareChunk(worker);
    const size_t before = frm.capacity();
    const size_t backendReleased = UncommitterTestAccess::Uncommit(worker);
    std::fprintf(stderr,
                 "DETAIL backendReleased=%zu cancelFirst=%d unit=%zu capacity before=%zu after=%zu\n",
                 backendReleased, cancelFirst ? 1 : 0, ZGranuleSize, before, frm.capacity());
    std::fflush(stderr);
    // ZPartition::decrease_capacity followed the uncommit (zUncommitter.cpp:417-419).
    GC_EXPECT_EQ(frm.capacity(), before - backendReleased);
    return backendReleased;
}

GC_COMPONENT_OTHER_VM_TEST(Uncommitter, UncommitIdleUnitsReleasesPhysical)
{
    const size_t backendReleased = ProbeProductUncommit(false);
    std::fprintf(stderr, "TARGET_UNCOMMIT_RELEASE_ASSERT bytes=%zu\n", backendReleased);
    GC_EXPECT_EQ(backendReleased, ZGranuleSize);
}

static void ExercisePartitionWorker(bool enabled)
{
    GC_EXPECT_EQ(setenv("cjUncommitDelay", enabled ? "10ms" : "0", 1), 0);
    BindUncommitWorkerThread();
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    RegionSpace& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& regions = space.GetRegionManager();
    space.GetRegionManager().freeRegionManager.StopUncommitters();
    const size_t n = 64 * MB / ZGranuleSize;
    ZPage* region = regions.TakeRegion((n) * ZGranuleSize, ZPageType::large, true, false);
    GC_EXPECT_TRUE(region != nullptr);
    regions.ReturnPageMemory(PageMemory{region->granule_index(), n * ZGranuleSize, 0, true});
    const size_t beforeReclaim = regions.GetCommittedCapacity();
    space.ReclaimGarbageMemory(true);
    GC_EXPECT_EQ(regions.GetCommittedCapacity(), beforeReclaim);
    const size_t before = regions.GetCommittedCapacity();
    const uint64_t start = TimeUtil::NanoSeconds();
    Uncommitter& worker = UncommitterTestAccess::Current();
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
    GC_EXPECT_TRUE(after >= UncommitterTestAccess::Partition().minCapacity);
    if (enabled) {
        GC_EXPECT_TRUE(after < before);
        GC_EXPECT_TRUE(observed - start >= Uncommitter::DelayNs());
        GC_EXPECT_TRUE(after >= UncommitterTestAccess::Partition().minCapacity);
    } else {
        GC_EXPECT_EQ(after, before);
    }
}

GC_RUNTIME_OTHER_VM_TEST(Uncommitter, TestUncommitIndependentPartitionThread)
{
    ExercisePartitionWorker(true);
}

GC_RUNTIME_OTHER_VM_TEST(Uncommitter, TestNoUncommitDisabledPartitionThread)
{
    ExercisePartitionWorker(false);
}

GC_COMPONENT_OTHER_VM_TEST(Uncommitter, CancelDelaysActivation)
{
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    CreateStandaloneHeap(64 * MB / ZGranuleSize);
    UncommitterTestAccess::ResetCancel();
    UncommitterTestAccess::Cancel();
    Uncommitter& worker = UncommitterTestAccess::Current();
    GC_EXPECT_FALSE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_TRUE(UncommitterTestAccess::Canceled());
}

GC_COMPONENT_OTHER_VM_TEST(Uncommitter, PeriodicUncommitStopsAfterCancel)
{
    const size_t backendReleased = ProbeProductUncommit(true);
    GC_EXPECT_EQ(backendReleased, 0U);
}

// ZGC zUncommitter.cpp:227-242: allocations lower the cycle watermark;
// returning that memory cannot increase the current cycle's budget.
GC_COMPONENT_OTHER_VM_TEST(Uncommitter, CacheValleyLimitsActivationBudget)
{
    BindUncommitWorkerThread();
    ProbeHeap heap(64 * MB / ZGranuleSize);
    auto& regions = Heap::GetHeap().GetAllocator().GetRegionManager();
    auto& frm = regions.freeRegionManager;
    InitializeUncommitCache(frm, heap);
    auto& partition = UncommitterTestAccess::Partition();
    auto& worker = partition.uncommitter;
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    std::fprintf(stderr, "TARGET_INITIAL_WATERMARK budget=%zu\n", UncommitterTestAccess::Budget(worker));
    GC_EXPECT_EQ(UncommitterTestAccess::Budget(worker), 0U);

    const size_t total = partition.capacity;
    const size_t allocated = total - 10 * ZGranuleSize;
    ZPage* page = regions.TakeRegion(allocated, ZPageType::large, true, false);
    GC_EXPECT_TRUE(page != nullptr);
    regions.ReturnPageMemory(PageMemory{page->granule_index(), allocated, 0, true});
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    const size_t actual = UncommitterTestAccess::Budget(worker);
    std::fprintf(stderr, "TARGET_CACHE_VALLEY budget=%zu expected=%zu capacity=%zu\n",
                 actual, 9 * ZGranuleSize, partition.capacity);
    GC_EXPECT_EQ(actual, 9 * ZGranuleSize);
    GC_EXPECT_EQ(partition.cache.min_size_watermark(), total);

    // The partition capacity floor wins when it is stricter than cache history.
    partition.minCapacity = total - ZGranuleSize;
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    std::fprintf(stderr, "TARGET_PARTITION_FLOOR budget=%zu\n", UncommitterTestAccess::Budget(worker));
    GC_EXPECT_EQ(UncommitterTestAccess::Budget(worker), ZGranuleSize);
}

// Real worker entry: a freshly filled cache starts with a zero historical
// watermark. Its first activation only resets history; a later cycle reclaims.
GC_RUNTIME_OTHER_VM_TEST(Uncommitter, FreshCacheWaitsForWatermarkCycle)
{
    GC_EXPECT_EQ(setenv("cjUncommitDelay", "400ms", 1), 0);
    BindUncommitWorkerThread();
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();
    auto& regions = Heap::GetHeap().GetAllocator().GetRegionManager();
    regions.freeRegionManager.StopUncommitters();
    auto& partition = UncommitterTestAccess::Partition();
    ZPage* page = regions.TakeRegion(64 * MB, ZPageType::large, true, false);
    GC_EXPECT_TRUE(page != nullptr);
    regions.ReturnPageMemory(PageMemory{page->granule_index(), 64 * MB, 0, true});
    UncommitterTestAccess::ResetCancel();
    const size_t before = regions.GetCommittedCapacity();
    partition.uncommitter.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const size_t firstCycle = regions.GetCommittedCapacity();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (regions.GetCommittedCapacity() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    partition.uncommitter.Stop();
    const size_t after = regions.GetCommittedCapacity();
    std::fprintf(stderr, "TARGET_FRESH_CACHE before=%zu first=%zu later=%zu\n", before, firstCycle, after);
    GC_EXPECT_EQ(firstCycle, before);
    GC_EXPECT_TRUE(after < before);
    const size_t stopped = regions.GetCommittedCapacity();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    GC_EXPECT_EQ(regions.GetCommittedCapacity(), stopped);
}

// ZGC zUncommitter.cpp:383-395: the activation budget is only an upper
// bound. Allocations during the cycle can reduce the remaining allowance.
GC_COMPONENT_OTHER_VM_TEST(Uncommitter, AllocationDuringCycleLowersUncommitAllowance)
{
    BindUncommitWorkerThread();
    ProbeHeap heap(64 * MB / ZGranuleSize);
    auto& regions = Heap::GetHeap().GetAllocator().GetRegionManager();
    InitializeUncommitCache(regions.freeRegionManager, heap);
    auto& partition = UncommitterTestAccess::Partition();
    auto& worker = partition.uncommitter;
    UncommitterTestAccess::ResetCancel();
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    GC_EXPECT_TRUE(UncommitterTestAccess::Activate(worker));
    const size_t before = partition.capacity;
    const size_t allocated = before - 2 * ZGranuleSize;
    ZPage* page = regions.TakeRegion(allocated, ZPageType::large, true, false);
    GC_EXPECT_TRUE(page != nullptr);
    regions.ReturnPageMemory(PageMemory{page->granule_index(), allocated, 0, true});
    size_t released = 0;
    for (int chunk = 0; chunk < 3; ++chunk) {
        released += UncommitterTestAccess::Uncommit(worker);
    }
    std::fprintf(stderr, "TARGET_CURRENT_WATERMARK released=%zu capacity=%zu expected=%zu\n",
                 released, partition.capacity, 2 * ZGranuleSize);
    GC_EXPECT_EQ(released, 2 * ZGranuleSize);
    GC_EXPECT_EQ(partition.capacity, before - released);
}
