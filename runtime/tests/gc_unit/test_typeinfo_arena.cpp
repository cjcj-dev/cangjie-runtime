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
#include <cstdio>
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

    struct FastMapShape {
        size_t entries;
        size_t capacity;
        size_t totalDistance;
        size_t maxDistance;
        bool lookupsExact;
    };

    // Arena-shaped generic keys: 8-byte aligned TypeTemplate and TypeInfo addresses with the
    // sizeof(TypeInfo) stride CreatedTypeInfoImpl uses. The fast map only hashes and compares
    // key and value pointers, so these addresses are never dereferenced.
    static void FastMapKey(size_t index, TypeTemplate*& tt, U32& argSize, TypeInfo* args[])
    {
        constexpr uintptr_t templateBase = 0x7f3a12345000ULL;
        constexpr uintptr_t arenaBase = 0x7f3b00000000ULL;
        constexpr uintptr_t typeInfoStride = 96;
        tt = reinterpret_cast<TypeTemplate*>(templateBase + (index % 64) * 64);
        argSize = index % 3 == 0 ? 2 : 1;
        for (U32 arg = 0; arg < argSize; ++arg) {
            args[arg] = reinterpret_cast<TypeInfo*>(arenaBase + (index * 2 + arg) * typeInfoStride);
        }
    }

    static TypeInfoManager::GenericTiDesc* FastMapValue(size_t index)
    {
        return reinterpret_cast<TypeInfoManager::GenericTiDesc*>(0x7f3c00000000ULL + index * 64);
    }

    // Publishes keyCount entries through the product Get/Insert generation handshake, checks every
    // lookup, and reports how far each published entry sits from its home slot.
    static FastMapShape FillFastMap(size_t keyCount, size_t initialCapacity)
    {
        TypeInfoManager::GenericTiDescFastMap map(initialCapacity);
        TypeTemplate* tt = nullptr;
        U32 argSize = 0;
        TypeInfo* args[2] = { nullptr, nullptr };
        bool exact = true;
        for (size_t index = 0; index < keyCount; ++index) {
            FastMapKey(index, tt, argSize, args);
            U64 generation = 0;
            exact = exact && map.Get(tt, argSize, args, generation) == nullptr;
            map.Insert(tt, argSize, args, FastMapValue(index), generation);
        }
        for (size_t index = 0; index < keyCount; ++index) {
            FastMapKey(index, tt, argSize, args);
            U64 generation = 0;
            exact = exact && map.Get(tt, argSize, args, generation) == FastMapValue(index);
        }
        FastMapKey(keyCount, tt, argSize, args);
        U64 generation = 0;
        exact = exact && map.Get(tt, argSize, args, generation) == nullptr;

        auto* table = map.activeTable.load(std::memory_order_acquire);
        const size_t mask = table->capacity - 1;
        FastMapShape shape { 0, table->capacity, 0, 0, exact };
        for (size_t slot = 0; slot < table->capacity; ++slot) {
            auto* entry = table->slots[slot].load(std::memory_order_acquire);
            if (entry == nullptr) {
                continue;
            }
            const size_t distance = (slot - (entry->hash & mask)) & mask;
            ++shape.entries;
            shape.totalDistance += distance;
            shape.maxDistance = std::max(shape.maxDistance, distance);
        }
        return shape;
    }

    // Unload invalidation must keep dropping every cached descriptor and refusing inserts that
    // observed an older generation.
    static bool FastMapInvalidationHolds()
    {
        TypeInfoManager::GenericTiDescFastMap map(8);
        TypeTemplate* tt = nullptr;
        U32 argSize = 0;
        TypeInfo* args[2] = { nullptr, nullptr };
        FastMapKey(1, tt, argSize, args);
        U64 before = 0;
        bool holds = map.Get(tt, argSize, args, before) == nullptr;
        map.Insert(tt, argSize, args, FastMapValue(1), before);
        U64 observed = 0;
        holds = holds && map.Get(tt, argSize, args, observed) == FastMapValue(1) && observed == before;
        map.Invalidate();
        U64 after = 0;
        holds = holds && map.Get(tt, argSize, args, after) == nullptr && after != before;
        map.Insert(tt, argSize, args, FastMapValue(1), before);
        holds = holds && map.Get(tt, argSize, args, observed) == nullptr;
        map.Insert(tt, argSize, args, FastMapValue(1), after);
        holds = holds && map.Get(tt, argSize, args, observed) == FastMapValue(1);
        return holds;
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

GC_RUNTIME_OTHER_VM_TEST(TypeInfoArena, UUIDColdAndWarmLookup)
{
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);

    DYN_TypeInfo type {};
    type.typeInfoName = "gc.uuid:ColdAndWarmLookup";
    type.type = TypeKind::TYPE_KIND_INT64;
    type.instanceSize = sizeof(int64_t);
    type.align = sizeof(int64_t);
    auto* ti = reinterpret_cast<TypeInfo*>(&type);
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    GC_EXPECT_EQ(type.uuid, 0U);
    GC_EXPECT_FALSE(manager.ContainsTypeInfo(ti));

    // Both calls enter the product GetUUID symbol, rather than a copy of
    // its inline implementation in this test executable.
    const U32 first = ti->GetUUID();
    GC_EXPECT_NE(first, 0U);
    GC_EXPECT_EQ(type.uuid, first);
    GC_EXPECT_TRUE(manager.ContainsTypeInfo(ti));
    const U32 warm = ti->GetUUID();
    GC_EXPECT_EQ(warm, first);
    std::fprintf(stderr, "UUID_LOOKUP_TARGET cold_registered=1 warm_same=1 uuid=%u\n", first);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// GenericTiDescFastMap::Get is the GetOrCreateTypeInfo hit path. Its slot index is the low bits of
// the key hash, so aligned metadata addresses must still spread over every slot; otherwise each hit
// walks a linear-probe run and dereferences every non-matching entry on the way.
GC_TEST(TypeInfoFastMap, AlignedKeysStayNearHomeSlot)
{
    using MapleRuntime::TypeInfoManagerTestAccess;
    const auto atLoad = TypeInfoManagerTestAccess::FillFastMap(2000, 4096);
    const auto grown = TypeInfoManagerTestAccess::FillFastMap(3000, 8);
    std::printf("TypeInfoFastMap fixed: entries=%zu capacity=%zu totalDistance=%zu maxDistance=%zu\n",
        atLoad.entries, atLoad.capacity, atLoad.totalDistance, atLoad.maxDistance);
    std::printf("TypeInfoFastMap grown: entries=%zu capacity=%zu totalDistance=%zu maxDistance=%zu\n",
        grown.entries, grown.capacity, grown.totalDistance, grown.maxDistance);
    GC_EXPECT_TRUE(atLoad.lookupsExact);
    GC_EXPECT_TRUE(grown.lookupsExact);
    GC_EXPECT_EQ(atLoad.entries, static_cast<size_t>(2000));
    GC_EXPECT_EQ(grown.entries, static_cast<size_t>(3000));
    // Uniform hashing at these loads (0.49 and <= 0.7) averages well under one slot of displacement.
    GC_EXPECT_TRUE(atLoad.totalDistance <= atLoad.entries);
    GC_EXPECT_TRUE(grown.totalDistance <= grown.entries);
    GC_EXPECT_TRUE(atLoad.maxDistance <= 64);
    GC_EXPECT_TRUE(grown.maxDistance <= 64);
}

GC_TEST(TypeInfoFastMap, InvalidateDropsEntriesAndStaleInserts)
{
    GC_EXPECT_TRUE(MapleRuntime::TypeInfoManagerTestAccess::FastMapInvalidationHolds());
}
