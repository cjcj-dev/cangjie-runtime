// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>
#include "gc_unittest.hpp"
#include "Heap/Allocator/CartesianTree.h"
#define private public
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#undef private
#include "Cangjie.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/z_globals.hpp"
#include "Mutator/ThreadLocal.h"
#include "gc_allocation_flags.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
RegionManager& InitAllocationRuntime(size_t heapKiB = 64 * 1024)
{
    setenv("cjUncommit", "0", 1);
    setenv("cjEnableGC", "0", 1);
    RuntimeParam params{};
    params.heapParam.heapSize = heapKiB;
    params.coParam.processorNum = 1;
    params.gcParam.parallelGCThreads = 1;
    params.gcParam.parallelGCThreadsSet = true;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    return Heap::GetHeap().page_allocator();
}
ZPage* Allocate(size_t size, PageAge age = PageAge::old)
{
    return Heap::alloc_page(size, size == ZPageSizeSmall ? ZPageType::small : ZPageType::large,
                            age, NonBlockingAllocationFlags());
}
struct Account {
    size_t capacity, used, max, cache;
    explicit Account(RegionManager& manager)
        : capacity(manager.capacity()), used(manager.GetUsedBytes()), max(manager.current_max_capacity()),
          cache(manager.GetCachedBytes()) {}
    void CheckUnchanged(RegionManager& manager) const
    {
        std::fprintf(stderr, "ACCOUNT_TARGET capacity=%zu/%zu used=%zu/%zu max=%zu/%zu cache=%zu/%zu\n",
            manager.capacity(), capacity, manager.GetUsedBytes(), used, manager.current_max_capacity(), max,
            manager.GetCachedBytes(), cache);
        GC_EXPECT_EQ(manager.capacity(), capacity);
        GC_EXPECT_EQ(manager.GetUsedBytes(), used);
        GC_EXPECT_EQ(manager.current_max_capacity(), max);
        GC_EXPECT_EQ(manager.GetCachedBytes(), cache);
    }
};
}

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, AddressIntactAllocates)
{
    auto& manager = InitAllocationRuntime();
    const size_t used = manager.GetUsedBytes();
    ZPage* page = Allocate(16 * MB);
    std::fprintf(stderr, "ADDRESS_INTACT_TARGET page=%p used=%zu\n", page, manager.GetUsedBytes());
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_EQ(Heap::page(page->GetRegionStart()), page);
    GC_EXPECT_EQ(manager.GetUsedBytes(), used + 16 * MB);
    *reinterpret_cast<uint64_t*>(page->GetRegionStart() + 4096) = 0x1317;
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(page->GetRegionStart() + 4096), uint64_t{0x1317});
    Heap::free_page(page);
    GC_EXPECT_EQ(manager.GetUsedBytes(), used);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, AddressExhaustionReturnsNull)
{
    auto& manager = InitAllocationRuntime();
    ZArray<ZVirtualMemory> saved;
    // ZGC test_zVirtualMemoryManager: remove the partition registry's supply.
    manager.virtualMemory->remove_from_low_many_at_most(ZAddressOffsetMax, 0, &saved);
    GC_EXPECT_FALSE(saved.is_empty());
    const Account before(manager);
    std::fprintf(stderr, "ADDRESS_EXHAUSTION_ENTER requested=%zu saved=%d\n", 16 * MB, saved.length());
    ZPage* page = Allocate(16 * MB);
    std::fprintf(stderr, "ADDRESS_NULL_TARGET page=%p\n", page);
    GC_EXPECT_TRUE(page == nullptr);
    before.CheckUnchanged(manager);
    for (const auto vmem : saved) { manager.virtualMemory->insert(vmem, 0); }
    page = Allocate(16 * MB);
    GC_EXPECT_TRUE(page != nullptr);
    Heap::free_page(page);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, HarvestAddressFailureRestores)
{
    auto& manager = InitAllocationRuntime();
    std::vector<ZPage*> pages;
    while (ZPage* page = Allocate(ZPageSizeSmall)) { pages.push_back(page); }
    GC_EXPECT_TRUE(pages.size() >= 8);
    std::vector<uintptr_t> payloads;
    std::vector<uint64_t> expectedPayloads;
    for (size_t i : {size_t{0}, size_t{2}, size_t{4}, size_t{6}}) {
        uintptr_t payload = pages[i]->GetRegionStart() + 4096;
        *reinterpret_cast<uint64_t*>(payload) = 0x1317 + i;
        payloads.push_back(payload);
        expectedPayloads.push_back(*reinterpret_cast<uint64_t*>(payload));
        Heap::free_page(pages[i]);
        pages[i] = nullptr;
    }
    ZArray<ZVirtualMemory> saved;
    manager.virtualMemory->remove_from_low_many_at_most(ZAddressOffsetMax, 0, &saved);
    const Account before(manager);
    ZPage* result = Allocate(4 * ZGranuleSize);
    std::fprintf(stderr, "HARVEST_ADDRESS_TARGET result=%p cached=%zu\n", result, manager.GetCachedBytes());
    GC_EXPECT_TRUE(result == nullptr);
    before.CheckUnchanged(manager);
    // ZGC zPhysicalMemoryManager.cpp:369-394 sorts stashed physical segments
    // before restoring them. Content belongs to the recovered set, not a fixed VA.
    std::vector<uint64_t> actualPayloads;
    for (const uintptr_t payload : payloads) {
        actualPayloads.push_back(*reinterpret_cast<uint64_t*>(payload));
    }
    std::sort(expectedPayloads.begin(), expectedPayloads.end());
    std::sort(actualPayloads.begin(), actualPayloads.end());
    std::fprintf(stderr, "HARVEST_PAYLOAD_SET_TARGET mappings=%zu equal=%d\n",
                 actualPayloads.size(), actualPayloads == expectedPayloads);
    GC_EXPECT_TRUE(actualPayloads == expectedPayloads);
    for (const auto vmem : saved) { manager.virtualMemory->insert(vmem, 0); }
    for (auto page : pages) { if (page != nullptr) { Heap::free_page(page); } }
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
void PartialCommit(bool checkMax)
{
    auto& manager = InitAllocationRuntime();
    const Account before(manager);
    const size_t requested = 16 * MB;
    const size_t limit = 4 * MB;
    // Q > C_before + L makes the retry fail its nonblocking capacity claim.
    GC_EXPECT_TRUE(requested > before.capacity + limit);
    GC_EXPECT_TRUE(before.max > before.capacity + requested);
    ZFailLargerCommits = limit;
    ZPage* page = Allocate(requested);
    ZFailLargerCommits = 0;
    const size_t committed = manager.capacity() - before.capacity;
    const size_t beforeCleanup = before.capacity + requested;
    const size_t remaining = requested - committed;
    std::fprintf(stderr, "PARTIAL_COMMIT_TARGET page=%p committed=%zu requested=%zu capacity_before_cleanup=%zu "
                         "remaining=%zu capacity_after=%zu max_after=%zu cache_before=%zu cache_after=%zu\n",
                         page, committed, requested, beforeCleanup, remaining, manager.capacity(),
                         manager.current_max_capacity(), before.cache, manager.GetCachedBytes());
    GC_EXPECT_TRUE(page == nullptr);
    GC_EXPECT_EQ(committed, limit);
    GC_EXPECT_TRUE(committed < requested);
    GC_EXPECT_EQ(manager.capacity(), beforeCleanup - remaining);
    GC_EXPECT_EQ(manager.GetUsedBytes(), before.used);
    GC_EXPECT_EQ(manager.GetCachedBytes(), before.cache + committed);
    if (checkMax) {
        std::fprintf(stderr, "PARTIAL_MAX_TARGET max=%zu capacity=%zu\n", manager.current_max_capacity(), manager.capacity());
        GC_EXPECT_EQ(manager.current_max_capacity(), manager.capacity());
    } else {
        // Only committed mappings are exposed by subsequent cache allocations.
        std::vector<ZPage*> cached;
        while (ZPage* next = Allocate(ZPageSizeSmall)) {
            *reinterpret_cast<uint64_t*>(next->GetRegionStart() + 4096) = 0x1317;
            cached.push_back(next);
        }
        std::fprintf(stderr, "PARTIAL_PREFIX_TARGET pages=%zu bytes=%zu\n", cached.size(), cached.size() * ZPageSizeSmall);
        GC_EXPECT_EQ(cached.size() * ZPageSizeSmall, before.cache + committed);
        for (auto next : cached) { Heap::free_page(next); }
    }
}
}
GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, PartialCommitAccounts) { PartialCommit(true); }
GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, PartialCommitPrefixPreserved) { PartialCommit(false); }

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, HarvestPartialCommitAccounts)
{
    auto& manager = InitAllocationRuntime();
    std::vector<ZPage*> occupied;
    for (size_t i = 0; i < 24; ++i) {
        ZPage* page = Allocate(ZPageSizeSmall);
        GC_EXPECT_TRUE(page != nullptr);
        occupied.push_back(page);
    }
    for (size_t i : {size_t{0}, size_t{2}, size_t{4}, size_t{6}}) {
        Heap::free_page(occupied[i]);
        occupied[i] = nullptr;
    }
    const Account before(manager);
    const size_t requested = 24 * MB;
    const size_t increased = manager.current_max_capacity() - before.capacity;
    const size_t harvested = requested - increased;
    const size_t limit = 4 * MB;
    GC_EXPECT_TRUE(harvested > 0 && increased > limit);
    GC_EXPECT_EQ(before.cache, harvested);
    ZFailLargerCommits = limit;
    ZPage* page = Allocate(requested);
    ZFailLargerCommits = 0;
    const size_t committed = manager.capacity() - before.capacity;
    std::fprintf(stderr, "HARVEST_PARTIAL_TARGET result=%p H=%zu I=%zu K=%zu freed=%zu C=%zu M=%zu\n",
                 page, harvested, increased, committed, manager.GetCachedBytes(), manager.capacity(), manager.current_max_capacity());
    GC_EXPECT_TRUE(page == nullptr);
    GC_EXPECT_EQ(committed, limit);
    GC_EXPECT_EQ(manager.GetCachedBytes(), harvested + committed);
    GC_EXPECT_EQ(manager.capacity(), before.capacity + increased - (requested - harvested - committed));
    GC_EXPECT_EQ(manager.current_max_capacity(), manager.capacity());
    GC_EXPECT_EQ(manager.GetUsedBytes(), before.used);
    for (auto retained : occupied) { if (retained != nullptr) Heap::free_page(retained); }
}

// TestCommitFailure.java:79-94 at page-allocation granularity: keep small
// allocations live while the larger request encounters the develop commit limit.
GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, CommitFailureRetainsSmallPages)
{
    auto& manager = InitAllocationRuntime();
    std::vector<ZPage*> retained;
    for (size_t i = 0; i < 4; ++i) {
        ZPage* page = Allocate(ZPageSizeSmall);
        GC_EXPECT_TRUE(page != nullptr);
        *reinterpret_cast<uint64_t*>(page->GetRegionStart() + 4096) = 0x131700 + i;
        retained.push_back(page);
    }
    const size_t used = manager.GetUsedBytes();
    ZFailLargerCommits = 4 * MB;
    ZPage* large = Allocate(16 * MB);
    ZFailLargerCommits = 0;
    std::fprintf(stderr, "COMMIT_FAILURE_RETAINED_TARGET large=%p used=%zu expected=%zu\n", large, manager.GetUsedBytes(), used);
    GC_EXPECT_TRUE(large == nullptr);
    GC_EXPECT_EQ(manager.GetUsedBytes(), used);
    for (size_t i = 0; i < retained.size(); ++i) {
        GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(retained[i]->GetRegionStart() + 4096), uint64_t{0x131700} + i);
        Heap::free_page(retained[i]);
    }
}
#endif

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, AllocationStallsTracksQueue)
{
    auto& manager = InitAllocationRuntime();
    // Hold the existing product driver lock so GC cannot consume the occupied pages.
    DriverLocker driverPause;
    std::vector<ZPage*> pages;
    while (ZPage* page = Allocate(ZPageSizeSmall)) { pages.push_back(page); }
    GC_EXPECT_FALSE(pages.empty());
    const size_t baseline = manager.Stats(ZGeneration::young()).allocation_stalls();
    std::atomic<ZPage*> result{nullptr};
    std::thread waiter([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        result.store(Heap::alloc_page(ZPageSizeSmall, ZPageType::small, PageAge::old, ZAllocationFlags{}));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    size_t during;
    do {
        during = manager.Stats(ZGeneration::young()).allocation_stalls();
        std::this_thread::yield();
    } while (during == baseline && std::chrono::steady_clock::now() < deadline);
    Heap::free_page(pages.back());
    pages.pop_back();
    waiter.join();
    const size_t after = manager.Stats(ZGeneration::young()).allocation_stalls();
    // The shared harness stops the VM after returning. Set its product abort
    // state before releasing the driver so the pending request exits at abortpoint.
    ZAbort::abort();
    std::fprintf(stderr, "STALL_STATS_TARGET before=%zu during=%zu after=%zu result=%p\n",
                 baseline, during, after, result.load());
    GC_EXPECT_EQ(during, baseline + 1);
    GC_EXPECT_EQ(after, baseline);
    GC_EXPECT_TRUE(result.load() != nullptr);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationTransaction, HeapAccountingSmallEden)
{
    auto& manager = InitAllocationRuntime(256 * 1024);
    auto& heap = Heap::GetHeap();
    // ZGC zTLABUsage.cpp:41-67 publishes history only at reset; observe live accounting here.
    const size_t before = heap._tlab_usage._used.load(std::memory_order_relaxed);
    const size_t used = manager.GetUsedBytes();
    const size_t youngBefore = manager.used_generation(ZGenerationId::young);
    ZPage* eden = Allocate(ZPageSizeSmall, PageAge::eden);
    GC_EXPECT_TRUE(eden != nullptr);
    std::fprintf(stderr, "HEAP_ACCOUNT_TARGET before=%zu allocated=%zu expected=%zu\n",
                 before, heap._tlab_usage._used.load(std::memory_order_relaxed), before + ZPageSizeSmall);
    GC_EXPECT_EQ(heap._tlab_usage._used.load(std::memory_order_relaxed), before + ZPageSizeSmall);
    heap.undo_alloc_page(eden);
    GC_EXPECT_EQ(heap._tlab_usage._used.load(std::memory_order_relaxed), before);
    GC_EXPECT_EQ(manager.GetUsedBytes(), used);
    GC_EXPECT_EQ(manager.used_generation(ZGenerationId::young), youngBefore);
    ZPage* old = Allocate(ZPageSizeSmall);
    ZPage* large = Allocate(8 * MB, PageAge::eden);
    ZPage* medium = Heap::alloc_page(ZPageSizeMediumMax, ZPageType::medium, PageAge::eden, NonBlockingAllocationFlags());
    GC_EXPECT_TRUE(old != nullptr && large != nullptr && medium != nullptr);
    GC_EXPECT_EQ(heap._tlab_usage._used.load(std::memory_order_relaxed), before);
    heap.undo_alloc_page(old);
    heap.undo_alloc_page(large);
    heap.undo_alloc_page(medium);
    GC_EXPECT_EQ(manager.GetUsedBytes(), used);
}
