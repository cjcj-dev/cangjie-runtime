// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>
#if !defined(_WIN64)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(__LP64__)
#include <cerrno>
#include <cstddef>
#include <linux/falloc.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include "Heap/Allocator/MemMap.h"
#include "Heap/Allocator/RegionManager.h"

namespace MapleRuntime {
namespace {

struct FakeMemMapBackend final : MemMapBackend {
    struct Op {
        uintptr_t start;
        size_t size;
        uint32_t node;
        bool bindNuma;
    };

    size_t maxReserveSize{ std::numeric_limits<size_t>::max() };
    size_t successfulReserveLimit{ std::numeric_limits<size_t>::max() };
    size_t failCommitCall{ 0 };
    size_t failReleaseCall{ 0 };
    size_t commitLimit{ std::numeric_limits<size_t>::max() };
    size_t releaseLimit{ std::numeric_limits<size_t>::max() };
    size_t commitCalls{ 0 };
    size_t releaseCalls{ 0 };
    uintptr_t nextBase{ 0x10000000U };
    size_t reserveCalls{ 0 };
    std::vector<Op> reserves;
    std::vector<Op> commits;
    std::vector<Op> protects;
    std::vector<Op> releases;
    std::vector<Op> unreserves;

    void* Reserve(void* requested, size_t size, unsigned int, const char*, bool exact) override
    {
        ++reserveCalls;
        if (size > maxReserveSize || reserves.size() >= successfulReserveLimit) {
            return nullptr;
        }
        uintptr_t start = requested == nullptr ? nextBase : reinterpret_cast<uintptr_t>(requested);
        if (exact && requested == nullptr) {
            return nullptr;
        }
        reserves.push_back(Op{ start, size, 0, false });
        nextBase = start + size + ALLOC_UTIL_PAGE_SIZE;
        return reinterpret_cast<void*>(start);
    }

    size_t Commit(void* addr, size_t size, int, uint32_t node, bool bindNuma) override
    {
        ++commitCalls;
        commits.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node, bindNuma });
        return failCommitCall == 0 || commitCalls != failCommitCall ? std::min(size, commitLimit) : 0;
    }

    bool Protect(void* addr, size_t size, int) override
    {
        protects.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, 0, false });
        return true;
    }

    size_t Release(void* addr, size_t size, uint32_t node) override
    {
        ++releaseCalls;
        releases.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node, false });
        return failReleaseCall == 0 || releaseCalls != failReleaseCall ? std::min(size, releaseLimit) : 0;
    }

    bool Unreserve(void* addr, size_t size) override
    {
        unreserves.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, 0, false });
        return true;
    }
};

AddressSpaceBudget LargeBudget()
{
    return AddressSpaceBudget::Seal(1U << 30U, 2);
}

NumaTopology OneNode()
{
    return NumaTopology::Seal({ 0 });
}

GC_TEST(MemMapContract, NullAndInvalidInputsDoNotReachOS)
{
    FakeMemMapBackend backend;
    MemMap* map = MemMap::TryMapMemory(0, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(), OneNode(), backend);
    GC_EXPECT_TRUE(map == nullptr);
    map = MemMap::TryMapMemory(ALLOC_UTIL_PAGE_SIZE, ALLOC_UTIL_PAGE_SIZE + 1,
                               MemMap::DEFAULT_OPTIONS, LargeBudget(), OneNode(), backend);
    GC_EXPECT_TRUE(map == nullptr);
    GC_EXPECT_EQ(backend.reserveCalls, 0U);
    MemMap::DestroyMemMap(map);
    GC_EXPECT_TRUE(map == nullptr);
}

GC_TEST(MemMapContract, AccessorsBoundaryCommitProtectAndDestroy)
{
    FakeMemMapBackend backend;
    const size_t requested = 3 * ALLOC_UTIL_PAGE_SIZE + 1;
    MemMap* map = MemMap::TryMapMemory(requested, ALLOC_UTIL_PAGE_SIZE, MemMap::DEFAULT_OPTIONS,
                                       LargeBudget(), OneNode(), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    GC_EXPECT_EQ(base, 0x10000000U);
    GC_EXPECT_EQ(map->GetCurrSize(), ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(map->GetCurrEnd()), base + ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(map->GetMappedSize(), 4U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(map->GetMappedEndAddr()), base + 4U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(map->GetReservationRegistry().TotalSize(), 4U * ALLOC_UTIL_PAGE_SIZE);

    GC_EXPECT_TRUE(map->CommitMemory(reinterpret_cast<void*>(base + ALLOC_UTIL_PAGE_SIZE), ALLOC_UTIL_PAGE_SIZE));
    GC_EXPECT_TRUE(map->ProtectMemory(reinterpret_cast<void*>(base), 2U * ALLOC_UTIL_PAGE_SIZE,
                                     MemMap::DEFAULT_MEM_PROT));
    GC_EXPECT_FALSE(map->CommitMemory(reinterpret_cast<void*>(base + 4U * ALLOC_UTIL_PAGE_SIZE), 1));
    GC_EXPECT_FALSE(map->ProtectMemory(reinterpret_cast<void*>(base - 1), 1, 0));

    MemMap::DestroyMemMap(map);
    GC_EXPECT_TRUE(map == nullptr);
    GC_EXPECT_EQ(backend.unreserves.size(), 1U);
    GC_EXPECT_EQ(backend.unreserves[0].start, base);
    GC_EXPECT_EQ(backend.unreserves[0].size, 4U * ALLOC_UTIL_PAGE_SIZE);
}

GC_TEST(MemMapContract, FallbackRegistryPreservesEveryAcquiredReservation)
{
    FakeMemMapBackend backend;
    backend.maxReserveSize = 2U * ALLOC_UTIL_PAGE_SIZE;
    const size_t total = 5U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(), OneNode(), backend,
                                       2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_TRUE(map != nullptr);
    const auto& ranges = map->GetReservationRegistry().Ranges();
    GC_EXPECT_EQ(map->GetReservationRegistry().TotalSize(), total);
    GC_EXPECT_EQ(ranges.size(), backend.reserves.size());
    size_t covered = 0;
    bool hasGap = false;
    for (size_t index = 0; index < ranges.size(); ++index) {
        GC_EXPECT_TRUE(ranges[index].start != 0);
        GC_EXPECT_TRUE(ranges[index].size != 0);
        GC_EXPECT_EQ(ranges[index].start % ALLOC_UTIL_PAGE_SIZE, 0U);
        GC_EXPECT_EQ(ranges[index].size % ALLOC_UTIL_PAGE_SIZE, 0U);
        covered += ranges[index].size;
        if (index != 0) {
            GC_EXPECT_TRUE(ranges[index - 1].End() <= ranges[index].start);
            hasGap = hasGap || ranges[index - 1].End() < ranges[index].start;
        }
        const size_t matches = static_cast<size_t>(std::count_if(backend.reserves.begin(), backend.reserves.end(),
            [&range = ranges[index]](const FakeMemMapBackend::Op& op) {
                return op.start == range.start && op.size == range.size;
            }));
        GC_EXPECT_EQ(matches, 1U);
    }
    GC_EXPECT_EQ(covered, total);
    GC_EXPECT_TRUE(hasGap);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(map->GetMappedEndAddr()), ranges.back().End());

    size_t committed = 0;
    for (const auto& range : ranges) {
        GC_EXPECT_TRUE(map->CommitMemory(reinterpret_cast<void*>(range.start), range.size));
        committed += range.size;
    }
    GC_EXPECT_EQ(committed, total);

    MemMap::DestroyMemMap(map);
    GC_EXPECT_EQ(backend.unreserves.size(), backend.reserves.size());
    for (const auto& reserved : backend.reserves) {
        const size_t count = static_cast<size_t>(std::count_if(backend.unreserves.begin(), backend.unreserves.end(),
            [&reserved](const FakeMemMapBackend::Op& op) {
                return op.start == reserved.start && op.size == reserved.size;
            }));
        GC_EXPECT_EQ(count, 1U);
    }

    FakeMemMapBackend rollbackBackend;
    rollbackBackend.maxReserveSize = 2U * ALLOC_UTIL_PAGE_SIZE;
    rollbackBackend.successfulReserveLimit = 1;
    map = MemMap::TryMapMemory(8U * ALLOC_UTIL_PAGE_SIZE, 0, MemMap::DEFAULT_OPTIONS,
                               LargeBudget(), OneNode(), rollbackBackend, 2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_TRUE(map == nullptr);
    GC_EXPECT_TRUE(!rollbackBackend.reserves.empty());
    GC_EXPECT_EQ(rollbackBackend.unreserves.size(), rollbackBackend.reserves.size());
    for (const auto& reserved : rollbackBackend.reserves) {
        const size_t count = static_cast<size_t>(std::count_if(
            rollbackBackend.unreserves.begin(), rollbackBackend.unreserves.end(),
            [&reserved](const FakeMemMapBackend::Op& op) {
                return op.start == reserved.start && op.size == reserved.size;
            }));
        GC_EXPECT_EQ(count, 1U);
    }
}

GC_TEST(MemMapContract, BudgetRejectsBeforeReserve)
{
    FakeMemMapBackend backend;
    const AddressSpaceBudget budget = AddressSpaceBudget::Seal(4U * ALLOC_UTIL_PAGE_SIZE, 2);
    MemMap* map = MemMap::TryMapMemory(3U * ALLOC_UTIL_PAGE_SIZE, 0, MemMap::DEFAULT_OPTIONS,
                                       budget, OneNode(), backend);
    GC_EXPECT_TRUE(map == nullptr);
    GC_EXPECT_EQ(backend.reserveCalls, 0U);
}

GC_TEST(MemMapContract, TwoNodeOwnershipRejectsCrossNodeFree)
{
    FakeMemMapBackend backend;
    const NumaTopology topology = NumaTopology::Seal({ 3, 7 });
    const size_t total = 4U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(), topology, backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    const auto& partitions = map->GetNumaPartitionRegistry().Ranges();
    GC_EXPECT_TRUE(!partitions.empty());
    size_t partitioned = 0;
    bool ownsNode3 = false;
    bool ownsNode7 = false;
    for (const auto& partition : partitions) {
        partitioned += partition.range.size;
        ownsNode3 = ownsNode3 || partition.node == 3U;
        ownsNode7 = ownsNode7 || partition.node == 7U;
    }
    GC_EXPECT_EQ(partitioned, total);
    GC_EXPECT_TRUE(ownsNode3);
    GC_EXPECT_TRUE(ownsNode7);

    GC_EXPECT_FALSE(map->ReleaseMemory(reinterpret_cast<void*>(base), ALLOC_UTIL_PAGE_SIZE, 7));
    GC_EXPECT_FALSE(map->ReleaseMemory(reinterpret_cast<void*>(base + ALLOC_UTIL_PAGE_SIZE),
                                      2U * ALLOC_UTIL_PAGE_SIZE, 3));
    GC_EXPECT_EQ(backend.releases.size(), 0U);
    GC_EXPECT_TRUE(map->ReleaseMemory(reinterpret_cast<void*>(base), 2U * ALLOC_UTIL_PAGE_SIZE, 3));
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    GC_EXPECT_TRUE(map->CommitMemory(reinterpret_cast<void*>(base), total));
    GC_EXPECT_TRUE(!backend.commits.empty());
    size_t committed = 0;
    bool sawNode3 = false;
    bool sawNode7 = false;
    for (const auto& commit : backend.commits) {
        committed += commit.size;
        sawNode3 = sawNode3 || commit.node == 3U;
        sawNode7 = sawNode7 || commit.node == 7U;
        GC_EXPECT_TRUE(commit.bindNuma);
    }
    GC_EXPECT_EQ(committed, total);
    GC_EXPECT_TRUE(sawNode3);
    GC_EXPECT_TRUE(sawNode7);

    MemMap::DestroyMemMap(map);
}

GC_TEST(MemMapContract, PartitionCommitRegistersPrefixAndExplicitRelease)
{
    FakeMemMapBackend backend;
    const size_t total = 2U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(),
                                       NumaTopology::Seal({ 3, 7 }), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    backend.failCommitCall = backend.commitCalls + 2;

    const size_t committed = map->CommitMemory(reinterpret_cast<void*>(base), total);
    GC_EXPECT_EQ(committed, ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(backend.commits.size(), 2U);
    GC_EXPECT_EQ(backend.commits[0].size, committed);

    GC_EXPECT_EQ(map->GetCommittedSize(), committed);
    const size_t cleaned = map->ReleaseMemory(reinterpret_cast<void*>(base), committed);
    GC_EXPECT_EQ(map->GetCommittedSize(), 0U);
    GC_EXPECT_EQ(cleaned, committed);
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    GC_EXPECT_EQ(backend.releases[0].size, cleaned);
    MemMap::DestroyMemMap(map);
}

// zPhysicalMemoryManager::commit/uncommit and zNMT::commit/uncommit:
// byte-count backend results include partial success within one partition.
GC_TEST(MemMapContract, BackingCapacityTracksOnlyCompletedRanges)
{
    FakeMemMapBackend backend;
    const size_t page = ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(4 * page, 0, MemMap::DEFAULT_OPTIONS,
                                     LargeBudget(), OneNode(), backend);
    GC_EXPECT_TRUE(map != nullptr);
    void* base = map->GetBaseAddr();
    backend.commitLimit = 2 * page;
    GC_EXPECT_EQ(map->CommitMemory(base, 4 * page), 2 * page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 2 * page);
    GC_EXPECT_EQ(map->CommitMemory(base, 2 * page), 2 * page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 2 * page);
    backend.commitLimit = 4 * page;
    GC_EXPECT_EQ(map->CommitMemory(base, 4 * page), 4 * page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 4 * page);
    backend.releaseLimit = page;
    GC_EXPECT_EQ(map->ReleaseMemory(base, 4 * page), page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 3 * page);
    GC_EXPECT_EQ(map->ReleaseMemory(base, page), page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 3 * page);
    backend.releaseLimit = 4 * page;
    GC_EXPECT_EQ(map->ReleaseMemory(base, 4 * page), 4 * page);
    GC_EXPECT_EQ(map->GetCommittedSize(), 0U);
    MemMap::DestroyMemMap(map);
}

GC_TEST(MemMapContract, PartitionReleaseReportsPrefixOnSecondFailure)
{
    FakeMemMapBackend backend;
    const size_t total = 2U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(),
                                       NumaTopology::Seal({ 3, 7 }), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base), total), total);
    backend.failReleaseCall = backend.releaseCalls + 2;

    const size_t released = map->ReleaseMemory(reinterpret_cast<void*>(base), total);
    GC_EXPECT_EQ(released, ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(backend.releases.size(), 2U);
    GC_EXPECT_EQ(backend.releases[0].size, released);
    MemMap::DestroyMemMap(map);
}

GC_TEST(MemMapContract, PartitionFirstFailureIsZeroPositiveControl)
{
    FakeMemMapBackend backend;
    const size_t total = 2U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(),
                                       NumaTopology::Seal({ 3, 7 }), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    backend.failCommitCall = backend.commitCalls + 1;
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base), total), 0U);
    GC_EXPECT_EQ(backend.commits.size(), 1U);
    GC_EXPECT_EQ(backend.releases.size(), 0U);
    MemMap::DestroyMemMap(map);
}

GC_TEST(MemMapContract, PartitionReleaseFirstFailureIsZeroPositiveControl)
{
    FakeMemMapBackend backend;
    const size_t total = 2U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(),
                                       NumaTopology::Seal({ 3, 7 }), backend);
    GC_EXPECT_TRUE(map != nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    GC_EXPECT_EQ(map->CommitMemory(reinterpret_cast<void*>(base), total), total);
    backend.failReleaseCall = backend.releaseCalls + 1;
    GC_EXPECT_EQ(map->ReleaseMemory(reinterpret_cast<void*>(base), total), 0U);
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    MemMap::DestroyMemMap(map);
}

GC_TEST(MemMapContract, InitialPartialCommitRollsBackPrefixBeforeDestroy)
{
    FakeMemMapBackend backend;
    MemMap::Option options = MemMap::DEFAULT_OPTIONS;
    options.protAll = true;
    const size_t total = 2U * ALLOC_UTIL_PAGE_SIZE;
    backend.failCommitCall = 2;

    MemMap* map = MemMap::TryMapMemory(total, 0, options, LargeBudget(), NumaTopology::Seal({ 3, 7 }), backend);
    GC_EXPECT_TRUE(map == nullptr);
    GC_EXPECT_EQ(backend.commits.size(), 2U);
    GC_EXPECT_EQ(backend.commits[0].size, ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    GC_EXPECT_EQ(backend.releases[0].size, ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(backend.unreserves.size(), 1U);
}

#if !defined(_WIN64)
struct ProductWiringBackend final : MemMapBackend {
    size_t commitCalls{ 0 };
    size_t failCommitCall{ 0 };
    int releaseEventFd{ -1 };

    void* Reserve(void* requested, size_t size, unsigned int, const char*, bool exact) override
    {
        void* result = mmap(requested, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED) {
            return nullptr;
        }
        if (exact && result != requested) {
            (void)munmap(result, size);
            return nullptr;
        }
        return result;
    }

    size_t Commit(void*, size_t size, int, uint32_t, bool) override
    {
        ++commitCalls;
        return failCommitCall == 0 || commitCalls != failCommitCall ? size : 0;
    }

    bool Protect(void*, size_t, int) override { return true; }
    size_t Release(void*, size_t size, uint32_t) override
    {
        if (releaseEventFd >= 0) {
            const ssize_t written = write(releaseEventFd, &size, sizeof(size));
            if (written != static_cast<ssize_t>(sizeof(size))) {
                return false;
            }
        }
        return size;
    }
    bool Unreserve(void* addr, size_t size) override { return munmap(addr, size) == 0; }
};

// Port of ZVirtualMemoryManagerTest::test_reserve_discontiguous_and_coalesce
// and test_remove_from_low: the backend deterministically exposes two real
// mappings separated by one inaccessible granule. RegionManager owns the
// allocation and materialization; the test reads its returned RegionInfo.
struct SegmentedProductBackend final : MemMapBackend {
    uintptr_t arena{ 0 };
    size_t reserved{ 0 };
    size_t unreserved{ 0 };
    std::vector<MemoryRange> commits;
    std::vector<MemoryRange> releases;

    SegmentedProductBackend()
    {
        void* mapping = mmap(nullptr, 5 * RegionInfo::UNIT_SIZE, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping != MAP_FAILED) {
            arena = reinterpret_cast<uintptr_t>(mapping);
        }
    }

    ~SegmentedProductBackend() override
    {
        if (arena != 0) {
            (void)munmap(reinterpret_cast<void*>(arena), 5 * RegionInfo::UNIT_SIZE);
        }
    }

    void* Reserve(void*, size_t size, unsigned int, const char*, bool) override
    {
        if (arena == 0 || size != 2 * RegionInfo::UNIT_SIZE || reserved == 2) {
            return nullptr;
        }
        return reinterpret_cast<void*>(arena + reserved++ * 3 * RegionInfo::UNIT_SIZE);
    }
    size_t Commit(void* addr, size_t size, int prot, uint32_t, bool) override
    {
        commits.push_back({ reinterpret_cast<uintptr_t>(addr), size });
        return mprotect(addr, size, prot) == 0 ? size : 0;
    }
    bool Protect(void* addr, size_t size, int prot) override { return mprotect(addr, size, prot) == 0; }
    size_t Release(void* addr, size_t size, uint32_t) override
    {
        releases.push_back({ reinterpret_cast<uintptr_t>(addr), size });
        return madvise(addr, size, MADV_DONTNEED) == 0 ? size : 0;
    }
    bool Unreserve(void*, size_t) override
    {
        ++unreserved;
        return true; // the fake backend retains its arena until destruction
    }
};

int ExerciseSegmentedProductAllocation()
{
    SegmentedProductBackend backend;
    const size_t unit = RegionInfo::UNIT_SIZE;
    MemMap* map = MemMap::TryMapMemory(4 * unit, 0, MemMap::DEFAULT_OPTIONS,
                                       LargeBudget(), OneNode(), backend, 2 * unit);
    if (map == nullptr) {
        return 10;
    }
    const auto& ranges = map->GetReservationRegistry().Ranges();
    const size_t metadataSize = RegionManager::GetMetadataSize(RegionInfo::IndexedUnitCount(ranges));
    MemMap* metadata = MemMap::MapMemory(metadataSize, metadataSize);
    int result = 0;
    {
        RegionManager manager;
        HeapParam heapParam{};
        heapParam.regionSize = 64;
        heapParam.exemptionThreshold = 0.8;
        manager.InitializeSegments(reinterpret_cast<uintptr_t>(metadata->GetBaseAddr()), ranges,
                                   *map, heapParam, 0.5);
        const uintptr_t hole = backend.arena + 2 * unit;
        Heap::OnHeapCreated(backend.arena, { { ranges[0].start, ranges[0].End() },
                                           { ranges[1].start, ranges[1].End() } });
        Heap::OnHeapExtended(ranges[1].End());
        if (Heap::IsHeapAddress(hole) || RegionInfo::TryGetRegionInfoAt(hole) != nullptr ||
            !Heap::IsHeapAddress(ranges[1].start)) {
            return 11;
        }
        const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        if (manager.TakeRegion(3, role, false, false, false) != nullptr || !backend.commits.empty()) {
            return 12;
        }
        RegionInfo* first = manager.TakeRegion(2, role, false, false, false);
        RegionInfo* second = manager.TakeRegion(2, role, false, false, false);
        if (first == nullptr || second == nullptr || first->GetRegionStart() != ranges[0].start ||
            second->GetRegionStart() != ranges[1].start ||
            RegionInfo::TryGetRegionInfoAt(second->GetRegionEnd() - 1) != second ||
            RegionInfo::TryGetRegionInfoAt(hole) != nullptr) {
            return 13;
        }
        if (manager.GetActiveUnitCount() != 4 || manager.GetInactiveUnitCount() != 0 ||
            manager.GetHeapCapacity() != 4 * unit || backend.commits.size() != 2) {
            return 14;
        }
        size_t pages = 0;
        manager.VisitPageOwners([&](RegionInfo*) { ++pages; });
        if (pages != 2 || map->ReleaseMemory(reinterpret_cast<void*>(hole), unit) != 0 ||
            map->ReleaseMemory(reinterpret_cast<void*>(second->GetRegionStart()), 2 * unit) != 2 * unit ||
            backend.releases.size() != 1 || backend.releases.front().start != ranges[1].start) {
            return 15;
        }
        // Hand-back goes through the allocator's existing cache. A request
        // larger than either segment must still fail after both are cached.
        manager.ReturnPageMemory({ first->GetUnitIdx(), 2, 0, true });
        manager.ReturnPageMemory({ second->GetUnitIdx(), 2, 0, true });
        if (RegionInfo::TryGetRegionInfoAt(ranges[0].start) != nullptr ||
            RegionInfo::TryGetRegionInfoAt(ranges[1].start) != nullptr) {
            return 19;
        }
        if (manager.TakeRegion(3, role, false, false, false) != nullptr) {
            return 16;
        }
        RegionInfo* reused = manager.TakeRegion(2, role, false, false, false);
        if (reused == nullptr || !map->GetReservationRegistry().Contains(reused->GetRegionStart(), 2 * unit)) {
            return 17;
        }
    }
    MemMap::DestroyMemMap(metadata);
    MemMap::DestroyMemMap(map);
    if (backend.unreserved != 2) {
        result = 18;
    }
    return result;
}

GC_TEST(MemMapContract, SegmentedRegionManagerAllocationLookupAndReturn)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        _exit(ExerciseSegmentedProductAllocation());
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

enum class RetirementPath { RETURN, RECLAIM, RELEASE, MARK_QUARANTINE };

// Lifecycle extension of ZVirtualMemoryManagerTest::test_remove_from_low:
// zPageTable.cpp:101-113 and zArray.inline.hpp:210-246 require the last
// iterator to consume deferred destruction. There is no standalone upstream
// ZSafeDelete gtest; these cases exercise that protocol via RegionManager.
int ExerciseSegmentedPageRetirement(RetirementPath path, bool concurrent)
{
    SegmentedProductBackend backend;
    const size_t unit = RegionInfo::UNIT_SIZE;
    MemMap* map = MemMap::TryMapMemory(4 * unit, 0, MemMap::DEFAULT_OPTIONS,
                                     LargeBudget(), OneNode(), backend, 2 * unit);
    if (map == nullptr) {
        return 20;
    }
    const auto& ranges = map->GetReservationRegistry().Ranges();
    const size_t metadataSize = RegionManager::GetMetadataSize(RegionInfo::IndexedUnitCount(ranges));
    MemMap* metadata = MemMap::MapMemory(metadataSize, metadataSize);
    int result = 0;
    {
        RegionManager manager;
        HeapParam heapParam{};
        heapParam.regionSize = 64;
        heapParam.exemptionThreshold = 0.8;
        manager.InitializeSegments(reinterpret_cast<uintptr_t>(metadata->GetBaseAddr()), ranges,
                                   *map, heapParam, 0.5);
        const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        RegionInfo* first = manager.TakeRegion(2, role, false, false, false);
        RegionInfo* second = manager.TakeRegion(2, role, false, false, false);
        if (first == nullptr || second == nullptr) {
            return 21;
        }
        const uintptr_t start = first->GetRegionStart();
        const uintptr_t end = first->GetRegionEnd();
        const auto life = first->GetRegionLifeId();
        const auto index = first->GetUnitIdx();
        const auto type = first->GetRegionType();
        size_t retired = 0;
        auto retire = [&] {
            switch (path) {
                case RetirementPath::RETURN:
                    manager.ReturnPageMemory({ index, 2, 0, true });
                    break;
                case RetirementPath::RECLAIM:
                    manager.ReclaimRegion(first);
                    break;
                case RetirementPath::RELEASE:
                    if (manager.ReleaseRegion(first) != 2 * unit) {
                        result = 22;
                    }
                    break;
                case RetirementPath::MARK_QUARANTINE:
                    manager.ReclaimRegionToMarkQuarantine(first);
                    break;
            }
            ++retired;
        };
        auto inspectRetiredPage = [&] {
            // Every granule is withdrawn, including the last byte of a
            // multi-unit page. The descriptor still describes its old life.
            for (uintptr_t address = start; address < end; address += unit) {
                if (RegionInfo::TryGetRegionInfoAt(address) != nullptr ||
                    RegionInfo::TryGetRegionInfoAt(address + unit - 1) != nullptr) {
                    result = 23;
                }
            }
            if (first->GetRegionEnd() != end || first->GetRegionLifeId() != life ||
                first->GetRegionType() != type || first->IsFreeRegion()) {
                result = 24;
            }
            if (manager.GetDirtyUnitCount() != 0 || manager.GetInactiveUnitCount() != 0 ||
                !backend.releases.empty()) {
                result = 25;
            }
            // Both reservations are owned; a retired page is not available
            // for cache allocation while either iterator can still read it.
            if (manager.TakeRegion(1, role, false, false, false) != nullptr) {
                result = 26;
            }
            if (RegionInfo::TryGetRegionInfoAt(second->GetRegionStart()) != second ||
                RegionInfo::TryGetRegionInfoAt(ranges[0].End()) != nullptr) {
                result = 27;
            }
        };
        manager.VisitPageOwners([&](RegionInfo* outer) {
            if (outer != first) {
                return;
            }
            manager.VisitPageOwners([&](RegionInfo* inner) {
                if (inner != first) {
                    return;
                }
                if (concurrent) {
                    std::thread reclaimer(retire);
                    reclaimer.join();
                } else {
                    retire();
                }
                inspectRetiredPage();
            });
            // Ending the nested iterator must not drain the queue while
            // this outer callback still holds the original descriptor.
            inspectRetiredPage();
        });
        if (retired != 1 || !first->IsFreeRegion() || first->GetRegionLifeId() == life) {
            result = 28;
        }
        if (path == RetirementPath::MARK_QUARANTINE) {
            manager.ReleaseMarkQuarantine();
        }
        if (path == RetirementPath::RELEASE) {
            if (manager.GetInactiveUnitCount() != 2 || backend.releases.size() != 1) {
                result = 29;
            }
        } else if (manager.GetDirtyUnitCount() != 2 || !backend.releases.empty()) {
            result = 30;
        }
        RegionInfo* reused = manager.TakeRegion(2, role, false, false, false);
        if (reused == nullptr || reused->GetRegionStart() != start ||
            RegionInfo::TryGetRegionInfoAt(end - 1) != reused) {
            result = 31;
        }
    }
    MemMap::DestroyMemMap(metadata);
    MemMap::DestroyMemMap(map);
    return result;
}

void CheckPageRetirement(RetirementPath path, bool concurrent)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        _exit(ExerciseSegmentedPageRetirement(path, concurrent));
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

GC_TEST(MemMapContract, PageTableReturnWaitsForOutermostIterator)
{
    CheckPageRetirement(RetirementPath::RETURN, false);
}

GC_TEST(MemMapContract, PageTableConcurrentReclaimPreservesDescriptor)
{
    CheckPageRetirement(RetirementPath::RECLAIM, true);
}

GC_TEST(MemMapContract, PageTableReleaseWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::RELEASE, true);
}

GC_TEST(MemMapContract, PageTableMarkQuarantineWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::MARK_QUARANTINE, false);
}

int ExerciseProductOwnerWiring()
{
    constexpr size_t units = 2;
    const size_t metadataSize = RegionManager::GetMetadataSize(units);
    const size_t totalSize = metadataSize + units * RegionInfo::UNIT_SIZE;
    ProductWiringBackend backend;
    MemMap* map = MemMap::TryMapMemory(totalSize, metadataSize, MemMap::DEFAULT_OPTIONS,
                                       LargeBudget(), OneNode(), backend);
    if (map == nullptr) {
        return 2;
    }
    bool wired = false;
    {
        RegionManager manager;
        HeapParam heapParam{};
        heapParam.regionSize = 64;
        heapParam.exemptionThreshold = 0.8;
        manager.Initialize(units, reinterpret_cast<uintptr_t>(map->GetBaseAddr()), *map, heapParam, 0.5);
        const size_t before = backend.commitCalls;
        RegionInfo* region = manager.TakeRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS, false, false);
        wired = region != nullptr && backend.commitCalls == before + 1;
    }
    MemMap::DestroyMemMap(map);
    return wired ? 0 : 3;
}

GC_TEST(MemMapContract, RegionManagerInactiveAllocationUsesMemMapOwner)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        _exit(ExerciseProductOwnerWiring());
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

// Behavioral port of gc/z/TestCommitFailure.java (Normal and ZFakeNUMA):
// a failed large allocation leaves its successfully committed memory reusable.
int ExerciseRegionPartialCommit(size_t failureCall)
{
    const size_t unit = RegionInfo::UNIT_SIZE;
    ProductWiringBackend backend;
    MemMap* map = MemMap::TryMapMemory(2 * unit, 0, MemMap::DEFAULT_OPTIONS,
                                       LargeBudget(), NumaTopology::Seal({ 3, 7 }), backend);
    if (map == nullptr) { return 2; }
    const auto& ranges = map->GetReservationRegistry().Ranges();
    const size_t metadataSize = RegionManager::GetMetadataSize(RegionInfo::IndexedUnitCount(ranges));
    MemMap* metadata = MemMap::MapMemory(metadataSize, metadataSize);
    {
        RegionManager manager;
        HeapParam heapParam{};
        heapParam.regionSize = 64;
        heapParam.exemptionThreshold = 0.8;
        manager.InitializeSegments(reinterpret_cast<uintptr_t>(metadata->GetBaseAddr()), ranges,
                                   *map, heapParam, 0.5);
        backend.failCommitCall = failureCall;
        const auto role = RegionInfo::UnitRole::SMALL_SIZED_UNITS;
        RegionInfo* large = manager.TakeRegion(2, role, false, false, false);
        const size_t expected = failureCall == 0 ? 2 : failureCall - 1;
        if ((large != nullptr) != (failureCall == 0)) { return 3; }
        if (manager.GetCommittedCapacity() != expected * unit ||
            map->GetCommittedSize() != manager.GetCommittedCapacity()) { return 4; }
        if (failureCall != 0) {
            if (manager.GetDirtyUnitCount() != expected ||
                manager.GetInactiveUnitCount() != 2 - expected) { return 5; }
            const size_t before = backend.commitCalls;
            backend.failCommitCall = 0;
            RegionInfo* small = manager.TakeRegion(1, role, false, false, false);
            if (small == nullptr) { return 6; }
            // A retained prefix is harvested without re-committing it.
            if (expected != 0 && backend.commitCalls != before) { return 7; }
            if (manager.GetCommittedCapacity() != unit) { return 8; }
            manager.ReturnPageMemory({ small->GetUnitIdx(), 1, 0, true });
        } else {
            manager.ReturnPageMemory({ large->GetUnitIdx(), 2, 0, true });
        }
    }
    MemMap::DestroyMemMap(metadata);
    MemMap::DestroyMemMap(map);
    return 0;
}

GC_TEST(MemMapContract, RegionManagerCommitResultsPreserveCapacityAndCache)
{
    for (size_t failureCall : { 0U, 1U, 2U }) {
        const pid_t child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child == 0) { _exit(ExerciseRegionPartialCommit(failureCall)); }
        int status = 0;
        GC_EXPECT_EQ(waitpid(child, &status, 0), child);
        GC_EXPECT_TRUE(WIFEXITED(status));
        GC_EXPECT_EQ(WEXITSTATUS(status), 0);
    }
}

#endif

#if defined(__linux__) && defined(__LP64__)
// Extend gc/z/TestCommitFailure.java's graceful-failure scenario to the native
// backing error branches in zPhysicalMemoryBacking_linux.cpp:509-690. The
// filter is confined to a child and changes kernel results, not product code.
void FilterFallocateError(bool punchHole, int error, uint32_t allowedLength)
{
    const uint32_t mode = punchHole ? FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE : 0;
    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fallocate, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, mode, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[3])),
        BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, allowedLength, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<uint32_t>(error)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program { static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0])),
                               instructions };
    GC_EXPECT_EQ(prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL), 0);
    GC_EXPECT_EQ(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program), 0);
}

void CheckNativeBackingErrors(int commitError, uint32_t commitLimitPages,
                              int releaseError, uint32_t releaseLimitPages,
                              size_t committedPages, size_t releasedPages)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        try {
            const size_t page = ALLOC_UTIL_PAGE_SIZE;
            MemMap* map = MemMap::MapMemory(4 * page, 0, MemMap::DEFAULT_OPTIONS,
                                            LargeBudget(), OneNode());
            if (commitError != 0) {
                FilterFallocateError(false, commitError, commitLimitPages * page);
            }
            if (releaseError != 0) {
                FilterFallocateError(true, releaseError, releaseLimitPages * page);
            }
            const size_t committed = map->CommitMemory(map->GetBaseAddr(), 4 * page);
            GC_EXPECT_EQ(committed, committedPages * page);
            GC_EXPECT_EQ(map->GetCommittedSize(), committed);
            // Observe that the returned backing really is mapped, and that
            // retained ranges are reused without changing capacity.
            auto* base = static_cast<volatile uint8_t*>(map->GetBaseAddr());
            for (size_t offset = 0; offset < committed; offset += page) {
                GC_EXPECT_EQ(base[offset], 0U);
                base[offset] = 0x5a;
            }
            if (committed != 0) {
                GC_EXPECT_EQ(map->CommitMemory(map->GetBaseAddr(), committed), committed);
                GC_EXPECT_EQ(base[committed - page], 0x5aU);
                const size_t released = map->ReleaseMemory(map->GetBaseAddr(), committed);
                GC_EXPECT_EQ(released, releasedPages * page);
                GC_EXPECT_EQ(map->GetCommittedSize(), committed - released);
            }
            MemMap::DestroyMemMap(map);
            _exit(0);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "native backing assertion: %s\n", failure.what());
            _exit(1);
        }
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

GC_TEST(MemMapContract, NativeBackingCommitAndUncommit)
{
    CheckNativeBackingErrors(0, 0, 0, 0, 4, 4);
}

GC_TEST(MemMapContract, NativeBackingEnosysUsesCompat)
{
    CheckNativeBackingErrors(ENOSYS, 0, 0, 0, 4, 4);
}

GC_TEST(MemMapContract, NativeBackingEopnotsuppUsesCompat)
{
    CheckNativeBackingErrors(EOPNOTSUPP, 0, 0, 0, 4, 4);
}

GC_TEST(MemMapContract, NativeBackingEintrSplitsCommit)
{
    CheckNativeBackingErrors(EINTR, 1, 0, 0, 4, 4);
}

GC_TEST(MemMapContract, NativeBackingBlockEintrReturnsZero)
{
    CheckNativeBackingErrors(EINTR, 0, 0, 0, 0, 0);
}

GC_TEST(MemMapContract, NativeBackingFailureRetainsPrefixDespitePunchError)
{
    CheckNativeBackingErrors(ENOSPC, 1, EIO, 0, 1, 0);
}

GC_TEST(MemMapContract, NativeBackingCommitFailureReturnsZero)
{
    CheckNativeBackingErrors(ENOSPC, 0, EIO, 0, 0, 0);
}

GC_TEST(MemMapContract, NativeBackingEintrSplitsUncommit)
{
    CheckNativeBackingErrors(0, 0, EINTR, 1, 4, 4);
}

GC_TEST(MemMapContract, NativeBackingUncommitErrorRetainsCapacity)
{
    CheckNativeBackingErrors(0, 0, EIO, 0, 4, 0);
}
#endif

} // namespace
} // namespace MapleRuntime
