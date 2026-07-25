// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

#include "ObjectModel/ExtensionData.h"

namespace MapleRuntime {
namespace {
void FirstFunction(void*) {}
void UpdatedFunction(void*) {}
void StableFunction(void*) {}

template<size_t Size, size_t Alignment>
TypeInfo* InitTypeInfo(std::array<std::byte, Size>& storage, const char* name, I8 type, U32 uuid)
{
    static_assert(Size == sizeof(TypeInfo));
    static_assert(Alignment == alignof(TypeInfo));
    TypeInfo* typeInfo = reinterpret_cast<TypeInfo*>(storage.data());
    typeInfo->SetName(name);
    typeInfo->SetType(type);
    typeInfo->SetUUID(uuid);
    return typeInfo;
}
} // namespace
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    U64 branchChecks = 0;
    U64 errors = 0;

    FuncPtr firstTable[] = { FirstFunction };
    FuncPtr updatedTable[] = { UpdatedFunction };
    FuncPtr stableTable[] = { StableFunction };
    ExtensionData updatedExtensionData {};
    ExtensionData stableExtensionData {};
    updatedExtensionData.UpdateFuncTable(1, firstTable);
    updatedExtensionData.SetFuncTableUpdated();
    stableExtensionData.UpdateFuncTable(1, stableTable);
    stableExtensionData.SetFuncTableUpdated();

    alignas(TypeInfo) std::array<std::byte, sizeof(TypeInfo)> interfaceStorage {};
    alignas(TypeInfo) std::array<std::byte, sizeof(TypeInfo)> superStorage {};
    alignas(TypeInfo) std::array<std::byte, sizeof(TypeInfo)> forwardedTempEnumStorage {};
    alignas(TypeInfo) std::array<std::byte, sizeof(TypeInfo)> standaloneTempEnumStorage {};
    TypeInfo* interfaceType = InitTypeInfo<sizeof(TypeInfo), alignof(TypeInfo)>(
        interfaceStorage, "MTableCacheHarness.Interface", TypeKind::TYPE_KIND_INTERFACE, 101);
    TypeInfo* superType = InitTypeInfo<sizeof(TypeInfo), alignof(TypeInfo)>(
        superStorage, "MTableCacheHarness.Super", TypeKind::TYPE_KIND_CLASS, 102);
    TypeInfo* forwardedTempEnum = InitTypeInfo<sizeof(TypeInfo), alignof(TypeInfo)>(
        forwardedTempEnumStorage, "MTableCacheHarness.ForwardedTempEnum", TypeKind::TYPE_KIND_TEMP_ENUM, 103);
    TypeInfo* standaloneTempEnum = InitTypeInfo<sizeof(TypeInfo), alignof(TypeInfo)>(
        standaloneTempEnumStorage, "MTableCacheHarness.StandaloneTempEnum", TypeKind::TYPE_KIND_TEMP_ENUM, 104);

    MTableDesc superMTable(0);
    superMTable.needsResolveInner = false;
    superMTable.needsResolveOuter = false;
    superMTable.mTable.emplace(
        interfaceType->GetUUID(), InheritFuncTable(&updatedExtensionData, interfaceType, 1));
    superType->SetMTableDesc(&superMTable);
    forwardedTempEnum->SetSuperTypeInfo(superType);

    if (forwardedTempEnum->GetMTable(interfaceType) != firstTable) {
        ++errors;
    }
    ++branchChecks;
    if (superMTable.GetCachedExtensionData(interfaceType->GetUUID()) != &updatedExtensionData) {
        ++errors;
    }
    ++branchChecks;

    updatedExtensionData.UpdateFuncTable(1, updatedTable);
    if (forwardedTempEnum->GetMTable(interfaceType) != updatedTable) {
        ++errors;
    }
    ++branchChecks;
    if (forwardedTempEnum->GetMTable(interfaceType) == firstTable) {
        ++errors;
    }
    ++branchChecks;

    MTableDesc standaloneMTable(0);
    standaloneMTable.needsResolveInner = false;
    standaloneMTable.needsResolveOuter = false;
    standaloneMTable.mTable.emplace(
        interfaceType->GetUUID(), InheritFuncTable(&stableExtensionData, interfaceType, 1));
    standaloneTempEnum->SetMTableDesc(&standaloneMTable);
    if (standaloneTempEnum->GetMTable(interfaceType) != stableTable) {
        ++errors;
    }
    ++branchChecks;
    if (standaloneTempEnum->GetSuperTypeInfo() != nullptr) {
        ++errors;
    }
    ++branchChecks;
    if (standaloneTempEnum->GetMTable(interfaceType) != stableTable) {
        ++errors;
    }
    ++branchChecks;

    MTableDesc collisionMTable(0);
    collisionMTable.CacheExtensionData(1, &updatedExtensionData);
    collisionMTable.CacheExtensionData(1 + MTableDesc::CACHE_SIZE, &stableExtensionData);
    if (collisionMTable.GetCachedExtensionData(1) != &updatedExtensionData) {
        ++errors;
    }
    ++branchChecks;
    if (collisionMTable.GetCachedExtensionData(1 + MTableDesc::CACHE_SIZE) != nullptr) {
        ++errors;
    }
    ++branchChecks;
    collisionMTable.CacheExtensionData(MTableDesc::CACHE_ENTRY_BUSY, &stableExtensionData);
    if (collisionMTable.GetCachedExtensionData(MTableDesc::CACHE_ENTRY_BUSY) != nullptr) {
        ++errors;
    }
    ++branchChecks;

    constexpr U32 ENTRY_COUNT = static_cast<U32>(MTableDesc::CACHE_SIZE);
    constexpr U32 WRITER_ROUNDS = 10000;
    std::array<ExtensionData, ENTRY_COUNT> concurrentExtensionData {};
    MTableDesc concurrentMTable(0);
    std::atomic<bool> start { false };
    std::atomic<bool> stop { false };
    std::atomic<U64> reads { 0 };
    std::atomic<U64> concurrentErrors { 0 };
    std::vector<std::thread> threads;
    for (U32 idx = 0; idx < ENTRY_COUNT; ++idx) {
        threads.emplace_back([&, idx]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (U32 round = 0; round < WRITER_ROUNDS; ++round) {
                concurrentMTable.CacheExtensionData(idx + 1, &concurrentExtensionData[idx]);
            }
        });
    }
    for (U32 idx = 0; idx < 16; ++idx) {
        threads.emplace_back([&, idx]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            U32 cursor = idx;
            while (!stop.load(std::memory_order_acquire)) {
                U32 entryIdx = cursor++ % ENTRY_COUNT;
                ExtensionData* found = concurrentMTable.GetCachedExtensionData(entryIdx + 1);
                if (found != nullptr && found != &concurrentExtensionData[entryIdx]) {
                    concurrentErrors.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (U32 idx = 0; idx < ENTRY_COUNT; ++idx) {
        threads[idx].join();
    }
    for (U32 idx = 0; idx < ENTRY_COUNT; ++idx) {
        if (concurrentMTable.GetCachedExtensionData(idx + 1) != &concurrentExtensionData[idx]) {
            ++errors;
        }
    }
    stop.store(true, std::memory_order_release);
    for (U32 idx = ENTRY_COUNT; idx < threads.size(); ++idx) {
        threads[idx].join();
    }
    errors += concurrentErrors.load(std::memory_order_relaxed);
    branchChecks += 3;

    const bool passed = errors == 0;
    std::cout << "MTABLE_CACHE_STRESS writers=" << ENTRY_COUNT
              << " readers=16 writer_rounds=" << WRITER_ROUNDS
              << " branch_checks=" << branchChecks
              << " reads=" << reads.load(std::memory_order_relaxed)
              << " errors=" << errors
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
