// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "TypeInfoManager.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace MapleRuntime {
// Test-only access keeps Allocate private in the product API while exercising
// the same bump/grow transaction used by CreatedTypeInfo.
class TypeInfoManagerTestAccess {
public:
    static uintptr_t Allocate(TypeInfoManager& manager, size_t size)
    {
        return manager.Allocate(size);
    }
};
} // namespace MapleRuntime

namespace {
struct Allocation {
    uintptr_t address;
    size_t size;
};

void CheckDisjoint(std::vector<Allocation> allocations)
{
    std::sort(allocations.begin(), allocations.end(), [](const Allocation& lhs, const Allocation& rhs) {
        return lhs.address < rhs.address;
    });
    for (size_t index = 1; index < allocations.size(); ++index) {
        GC_EXPECT_TRUE(allocations[index - 1].address + allocations[index - 1].size <= allocations[index].address);
    }
}
} // namespace

GC_RUNTIME_OTHER_VM_TEST(TypeInfoArena, ConcurrentBumpAndLargeRequest)
{
    using MapleRuntime::TypeInfoManager;
    using MapleRuntime::TypeInfoManagerTestAccess;

    TypeInfoManager& manager = TypeInfoManager::GetTypeInfoManager();
    manager.Fini();
    manager.NewMMap(1 * MB);

    // Two independent creator-shaped workers use different metadata sizes. The
    // allocator lock covers only cursor/growth state, so their ranges must be
    // disjoint without requiring a descriptor/global TypeInfo lock.
    constexpr size_t workerCount = 4;
    constexpr size_t allocationsPerWorker = 256;
    std::vector<Allocation> allocations;
    allocations.reserve(workerCount * allocationsPerWorker);
    std::mutex allocationsMutex;
    std::atomic<bool> alignmentFailure{false};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker] {
            for (size_t index = 0; index < allocationsPerWorker; ++index) {
                const size_t size = 24 + ((worker + index) % 5) * 16;
                const uintptr_t address = TypeInfoManagerTestAccess::Allocate(manager, size);
                if ((address & 7U) != 0) {
                    alignmentFailure.store(true, std::memory_order_release);
                }
                std::lock_guard<std::mutex> lock(allocationsMutex);
                allocations.push_back({ address, (size + 7U) & ~size_t(7U) });
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    GC_EXPECT_FALSE(alignmentFailure.load(std::memory_order_acquire));
    CheckDisjoint(std::move(allocations));

    // A request larger than the normal 1 MiB segment must receive a mapping
    // that covers its complete aligned extent, then continue at a later range.
    constexpr size_t largeRequest = 2 * MB + 24;
    const uintptr_t large = TypeInfoManagerTestAccess::Allocate(manager, largeRequest);
    const size_t largeAligned = (largeRequest + 7U) & ~size_t(7U);
    const uintptr_t afterLarge = TypeInfoManagerTestAccess::Allocate(manager, 64);
    GC_EXPECT_TRUE((large & 7U) == 0);
    GC_EXPECT_TRUE(afterLarge >= large + largeAligned);
    GC_EXPECT_TRUE(manager.ContainsAddress(large));
    GC_EXPECT_TRUE(manager.ContainsAddress(large + largeAligned - 8));
    GC_EXPECT_TRUE(manager.ContainsAddress(afterLarge));

    manager.Fini();
}
