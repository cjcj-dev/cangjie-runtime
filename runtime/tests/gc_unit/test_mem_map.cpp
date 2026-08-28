// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#if !defined(_WIN64)
#include <sys/wait.h>
#include <unistd.h>
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

    bool Commit(void* addr, size_t size, int, uint32_t node, bool bindNuma) override
    {
        ++commitCalls;
        commits.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node, bindNuma });
        return failCommitCall == 0 || commitCalls != failCommitCall;
    }

    bool Protect(void* addr, size_t size, int) override
    {
        protects.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, 0, false });
        return true;
    }

    bool Release(void* addr, size_t size, uint32_t node) override
    {
        ++releaseCalls;
        releases.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node, false });
        return failReleaseCall == 0 || releaseCalls != failReleaseCall;
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

GC_TEST(MemMapContract, PartitionCommitReportsPrefixAndCallerCleansIt)
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

    const size_t cleaned = map->ReleaseMemory(reinterpret_cast<void*>(base), committed);
    GC_EXPECT_EQ(cleaned, committed);
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    GC_EXPECT_EQ(backend.releases[0].size, cleaned);
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

    bool Commit(void*, size_t, int, uint32_t, bool) override
    {
        ++commitCalls;
        return true;
    }

    bool Protect(void*, size_t, int) override { return true; }
    bool Release(void*, size_t, uint32_t) override { return true; }
    bool Unreserve(void* addr, size_t size) override { return munmap(addr, size) == 0; }
};

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
#endif

} // namespace
} // namespace MapleRuntime
