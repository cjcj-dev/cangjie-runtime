// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Heap/Allocator/MemMap.h"

namespace MapleRuntime {
namespace {

struct FakeMemMapBackend final : MemMapBackend {
    struct Op {
        uintptr_t start;
        size_t size;
        uint32_t node;
    };

    bool rejectFirstReserve{ false };
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
        if (rejectFirstReserve && reserveCalls == 1) {
            return nullptr;
        }
        uintptr_t start = requested == nullptr ? nextBase : reinterpret_cast<uintptr_t>(requested);
        if (exact && requested == nullptr) {
            return nullptr;
        }
        reserves.push_back(Op{ start, size, 0 });
        nextBase = start + size + ALLOC_UTIL_PAGE_SIZE;
        return reinterpret_cast<void*>(start);
    }

    bool Commit(void* addr, size_t size, int, uint32_t node, bool) override
    {
        commits.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node });
        return true;
    }

    bool Protect(void* addr, size_t size, int) override
    {
        protects.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, 0 });
        return true;
    }

    bool Release(void* addr, size_t size, uint32_t node) override
    {
        releases.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, node });
        return true;
    }

    bool Unreserve(void* addr, size_t size) override
    {
        unreserves.push_back(Op{ reinterpret_cast<uintptr_t>(addr), size, 0 });
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

GC_TEST(MemMapContract, FallbackRegistryHasExactNonOverlappingLifetime)
{
    FakeMemMapBackend backend;
    backend.rejectFirstReserve = true;
    const size_t total = 5U * ALLOC_UTIL_PAGE_SIZE;
    MemMap* map = MemMap::TryMapMemory(total, 0, MemMap::DEFAULT_OPTIONS, LargeBudget(), OneNode(), backend,
                                       2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_TRUE(map != nullptr);
    const auto& ranges = map->GetReservationRegistry().Ranges();
    GC_EXPECT_EQ(ranges.size(), 3U);
    GC_EXPECT_EQ(map->GetReservationRegistry().TotalSize(), total);
    GC_EXPECT_EQ(ranges[0].size, 2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(ranges[1].size, 2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(ranges[2].size, ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(ranges[0].End(), ranges[1].start);
    GC_EXPECT_EQ(ranges[1].End(), ranges[2].start);

    MemMap::DestroyMemMap(map);
    GC_EXPECT_EQ(backend.unreserves.size(), 3U);
    for (const auto& reserved : backend.reserves) {
        const size_t count = static_cast<size_t>(std::count_if(backend.unreserves.begin(), backend.unreserves.end(),
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
    GC_EXPECT_EQ(partitions.size(), 2U);
    GC_EXPECT_EQ(partitions[0].node, 3U);
    GC_EXPECT_EQ(partitions[1].node, 7U);
    GC_EXPECT_EQ(partitions[0].range.size, 2U * ALLOC_UTIL_PAGE_SIZE);
    GC_EXPECT_EQ(partitions[0].range.End(), partitions[1].range.start);

    GC_EXPECT_FALSE(map->ReleaseMemory(reinterpret_cast<void*>(base), ALLOC_UTIL_PAGE_SIZE, 7));
    GC_EXPECT_FALSE(map->ReleaseMemory(reinterpret_cast<void*>(base + ALLOC_UTIL_PAGE_SIZE),
                                      2U * ALLOC_UTIL_PAGE_SIZE, 3));
    GC_EXPECT_EQ(backend.releases.size(), 0U);
    GC_EXPECT_TRUE(map->ReleaseMemory(reinterpret_cast<void*>(base), 2U * ALLOC_UTIL_PAGE_SIZE, 3));
    GC_EXPECT_EQ(backend.releases.size(), 1U);
    GC_EXPECT_TRUE(map->CommitMemory(reinterpret_cast<void*>(base), total));
    GC_EXPECT_EQ(backend.commits.size(), 2U);
    GC_EXPECT_EQ(backend.commits[0].node, 3U);
    GC_EXPECT_EQ(backend.commits[1].node, 7U);

    MemMap::DestroyMemMap(map);
}

} // namespace
} // namespace MapleRuntime
