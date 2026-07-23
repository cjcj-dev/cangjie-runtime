// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#define MRT_TYPEINFO_FAST_MAP_TEST 1
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
constexpr U32 KEY_COUNT = 3072;
constexpr U32 INVALIDATION_ROUNDS = 20;

struct Key {
    TypeTemplate* tt;
    U32 argSize;
    std::array<TypeInfo*, 4> args;
    std::unique_ptr<TypeInfoManager::TestGenericTiDesc> firstDesc;
    std::unique_ptr<TypeInfoManager::TestGenericTiDesc> secondDesc;
};

U32 NextRandom(U32& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
} // namespace
} // namespace MapleRuntime

int main(int argc, char** argv)
{
    using namespace MapleRuntime;
    using Desc = TypeInfoManager::TestGenericTiDesc;
    using Map = TypeInfoManager::TestGenericTiDescFastMap;
    const U32 requestedThreads = argc > 1 ? static_cast<U32>(std::strtoul(argv[1], nullptr, 10)) : 16;
    const U32 readerThreads = requestedThreads > 4 ? requestedThreads - 4 : 1;

    std::vector<std::max_align_t> templateTokens(KEY_COUNT);
    std::vector<std::max_align_t> argumentTokens(KEY_COUNT * 4);
    std::vector<Key> keys;
    keys.reserve(KEY_COUNT);
    for (U32 idx = 0; idx < KEY_COUNT; ++idx) {
        Key key {
            reinterpret_cast<TypeTemplate*>(&templateTokens[idx]),
            (idx % 3) + 1,
            {},
            std::make_unique<Desc>(),
            std::make_unique<Desc>()
        };
        for (U32 argIdx = 0; argIdx < key.args.size(); ++argIdx) {
            key.args[argIdx] = reinterpret_cast<TypeInfo*>(&argumentTokens[idx * 4 + argIdx]);
        }
        keys.emplace_back(std::move(key));
    }

    U64 branchChecks = 0;
    U64 errors = 0;
    Map matrixMap(8);
    U64 generation = 0;
    if (matrixMap.Get(keys[0].tt, 0, keys[0].args.data(), generation) != nullptr) {
        ++errors;
    }
    ++branchChecks;
    matrixMap.Insert(keys[0].tt, 0, keys[0].args.data(), keys[0].firstDesc.get(), generation);
    if (matrixMap.Get(keys[0].tt, 0, keys[0].args.data(), generation) != keys[0].firstDesc.get()) {
        ++errors;
    }
    ++branchChecks;
    for (U32 argSize = 1; argSize <= 3; ++argSize) {
        U64 observed = 0;
        if (matrixMap.Get(keys[argSize].tt, argSize, keys[argSize].args.data(), observed) != nullptr) {
            ++errors;
        }
        matrixMap.Insert(keys[argSize].tt, argSize, keys[argSize].args.data(),
            keys[argSize].firstDesc.get(), observed);
        if (matrixMap.Get(keys[argSize].tt, argSize, keys[argSize].args.data(), observed) !=
            keys[argSize].firstDesc.get()) {
            ++errors;
        }
        branchChecks += 2;
    }
    U64 observed = 0;
    if (matrixMap.Get(keys[1].tt, 1, keys[2].args.data(), observed) != nullptr) {
        ++errors;
    }
    ++branchChecks;
    if (matrixMap.Get(keys[2].tt, 1, keys[1].args.data(), observed) != nullptr) {
        ++errors;
    }
    ++branchChecks;
    matrixMap.Insert(keys[1].tt, 1, keys[1].args.data(), keys[1].secondDesc.get(), observed);
    if (matrixMap.Get(keys[1].tt, 1, keys[1].args.data(), observed) != keys[1].firstDesc.get()) {
        ++errors;
    }
    ++branchChecks;
    if (matrixMap.Get(keys[4].tt, 4, keys[4].args.data(), observed) != nullptr) {
        ++errors;
    }
    matrixMap.Insert(keys[4].tt, 4, keys[4].args.data(), keys[4].firstDesc.get(), observed);
    if (matrixMap.Get(keys[4].tt, 4, keys[4].args.data(), observed) != nullptr) {
        ++errors;
    }
    branchChecks += 2;
    matrixMap.Get(keys[5].tt, keys[5].argSize, keys[5].args.data(), observed);
    matrixMap.Invalidate();
    matrixMap.Insert(keys[5].tt, keys[5].argSize, keys[5].args.data(), keys[5].firstDesc.get(), observed);
    if (matrixMap.Get(keys[5].tt, keys[5].argSize, keys[5].args.data(), observed) != nullptr) {
        ++errors;
    }
    ++branchChecks;
    matrixMap.Insert(keys[5].tt, keys[5].argSize, keys[5].args.data(), keys[5].secondDesc.get(), observed);
    if (matrixMap.Get(keys[5].tt, keys[5].argSize, keys[5].args.data(), observed) !=
        keys[5].secondDesc.get()) {
        ++errors;
    }
    ++branchChecks;

    Map stressMap(8);
    std::atomic<bool> start { false };
    std::atomic<bool> stop { false };
    std::atomic<bool> transition { false };
    std::atomic<U32> epoch { 0 };
    std::atomic<U64> reads { 0 };
    std::atomic<U64> concurrentErrors { 0 };
    std::vector<std::thread> threads;
    for (U32 threadIdx = 0; threadIdx < 4; ++threadIdx) {
        threads.emplace_back([&, threadIdx]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (U32 idx = threadIdx; idx < KEY_COUNT; idx += 4) {
                U64 keyGeneration = 0;
                stressMap.Get(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(), keyGeneration);
                stressMap.Insert(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(),
                    keys[idx].firstDesc.get(), keyGeneration);
            }
        });
    }
    for (U32 threadIdx = 0; threadIdx < readerThreads; ++threadIdx) {
        threads.emplace_back([&, threadIdx]() {
            U32 random = 0x9e3779b9U ^ (threadIdx + 1U);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                U32 idx = NextRandom(random) % KEY_COUNT;
                bool beforeTransition = transition.load(std::memory_order_acquire);
                U32 beforeEpoch = epoch.load(std::memory_order_acquire);
                U64 keyGeneration = 0;
                Desc* found = stressMap.Get(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(), keyGeneration);
                U32 afterEpoch = epoch.load(std::memory_order_acquire);
                bool afterTransition = transition.load(std::memory_order_acquire);
                if (!beforeTransition && !afterTransition && beforeEpoch == afterEpoch && beforeEpoch != 0) {
                    Desc* expected = (beforeEpoch & 1U) == 0 ? keys[idx].secondDesc.get() : keys[idx].firstDesc.get();
                    if (found != expected) {
                        concurrentErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (U32 idx = 0; idx < 4; ++idx) {
        threads[idx].join();
    }
    epoch.store(1, std::memory_order_release);
    for (U32 round = 2; round <= INVALIDATION_ROUNDS + 1; ++round) {
        transition.store(true, std::memory_order_release);
        stressMap.Invalidate();
        for (U32 idx = 0; idx < KEY_COUNT; ++idx) {
            U64 keyGeneration = 0;
            stressMap.Get(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(), keyGeneration);
            Desc* desc = (round & 1U) == 0 ? keys[idx].secondDesc.get() : keys[idx].firstDesc.get();
            stressMap.Insert(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(), desc, keyGeneration);
        }
        epoch.store(round, std::memory_order_release);
        transition.store(false, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    for (U32 idx = 4; idx < threads.size(); ++idx) {
        threads[idx].join();
    }

    U32 finalEpoch = epoch.load(std::memory_order_acquire);
    for (U32 idx = 0; idx < KEY_COUNT; ++idx) {
        U64 keyGeneration = 0;
        Desc* found = stressMap.Get(keys[idx].tt, keys[idx].argSize, keys[idx].args.data(), keyGeneration);
        Desc* expected = (finalEpoch & 1U) == 0 ? keys[idx].secondDesc.get() : keys[idx].firstDesc.get();
        if (found != expected) {
            ++errors;
        }
    }
    branchChecks += 5;
    errors += concurrentErrors.load(std::memory_order_relaxed);
    const bool passed = errors == 0;
    std::cout << "TYPEINFO_FAST_MAP_STRESS threads=" << requestedThreads
              << " keys=" << KEY_COUNT
              << " branch_checks=" << branchChecks
              << " reads=" << reads.load(std::memory_order_relaxed)
              << " invalidations=" << INVALIDATION_ROUNDS
              << " errors=" << errors
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
