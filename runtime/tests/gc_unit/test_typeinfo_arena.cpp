// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Interpreter/RuntimeTypes.h"
#include "TypeInfoManager.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <cstring>
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

using namespace MapleRuntime;

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
    using MapleRuntime::MB;

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
    GC_EXPECT_TRUE(afterLarge + 64 <= large || large + largeAligned <= afterLarge);
    GC_EXPECT_TRUE(manager.ContainsAddress(large));
    GC_EXPECT_TRUE(manager.ContainsAddress(large + largeAligned - 8));
    GC_EXPECT_TRUE(manager.ContainsAddress(afterLarge));

    manager.Fini();
}

namespace {
// TypeTemplate is a runtime metadata record emitted by the compiler and has a
// deleted constructor. This layout mirrors that record only for the native
// product-path test; the call still enters GetOrCreateTypeInfo directly.
struct TestTypeTemplateLayout {
    const char* name;
    I8 type;
    I8 flag;
    U16 fieldNum;
    U16 typeArgsNum;
    std::atomic<U16> uuid;
    const GenericFunc* fieldFns;
    GenericFunc superFn;
    FuncRef finalizerMethod;
    ReflectInfo* reflectInfo;
    ExtensionData** vExtensionDataStart;
    U16 validInheritNum;
};

static_assert(sizeof(TestTypeTemplateLayout) == sizeof(TypeTemplate));

TypeInfo* ReturnFirstTypeInfo(TypeInfo** args)
{
    return args[0];
}

TypeTemplate* MakeRawArrayTemplate(void* storage, const char* name, U16 uuid, const GenericFunc* fieldFns)
{
    std::memset(storage, 0, sizeof(TestTypeTemplateLayout));
    auto* layout = reinterpret_cast<TestTypeTemplateLayout*>(storage);
    layout->name = name;
    layout->type = TypeKind::TYPE_KIND_RAWARRAY;
    layout->typeArgsNum = 1;
    layout->uuid.store(uuid, std::memory_order_relaxed);
    layout->fieldFns = fieldFns;
    return reinterpret_cast<TypeTemplate*>(storage);
}
} // namespace

GC_RUNTIME_OTHER_VM_TEST(TypeInfoArena, ConcurrentGenericCreation)
{
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);

    DYN_TypeInfo argument {};
    argument.typeInfoName = "std.core:Int64";
    argument.type = TypeKind::TYPE_KIND_INT64;
    argument.instanceSize = sizeof(int64_t);
    argument.uuid = 1;
    argument.align = sizeof(int64_t);
    TypeInfo* args[] = { reinterpret_cast<TypeInfo*>(&argument) };

    alignas(TypeTemplate) std::byte storageA[sizeof(TypeTemplate)];
    alignas(TypeTemplate) std::byte storageB[sizeof(TypeTemplate)];
    const GenericFunc fieldFns[] = { ReturnFirstTypeInfo };
    TypeTemplate* templateA = MakeRawArrayTemplate(storageA, "arena:A", 101, fieldFns);
    TypeTemplate* templateB = MakeRawArrayTemplate(storageB, "arena:B", 102, fieldFns);
    TypeInfo* created[2] = { nullptr, nullptr };
    std::thread first([&] { created[0] = TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(templateA, 1, args); });
    std::thread second([&] { created[1] = TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(templateB, 1, args); });
    first.join();
    second.join();

    GC_EXPECT_TRUE(created[0] != nullptr);
    GC_EXPECT_TRUE(created[1] != nullptr);
    GC_EXPECT_NE(reinterpret_cast<uintptr_t>(created[0]), reinterpret_cast<uintptr_t>(created[1]));
    GC_EXPECT_TRUE((reinterpret_cast<uintptr_t>(created[0]) & 7U) == 0);
    GC_EXPECT_TRUE((reinterpret_cast<uintptr_t>(created[1]) & 7U) == 0);
    GC_EXPECT_TRUE(TypeInfoManager::GetTypeInfoManager().ContainsAddress(
        reinterpret_cast<uintptr_t>(created[0])));
    GC_EXPECT_TRUE(TypeInfoManager::GetTypeInfoManager().ContainsAddress(
        reinterpret_cast<uintptr_t>(created[1])));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
