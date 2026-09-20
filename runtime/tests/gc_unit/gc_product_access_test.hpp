// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zCollectedHeap.hpp"
#include "Mutator/MutatorManager.h"
#include "Common/OopStorage.h"
namespace MapleRuntime {
class ZCollectedHeapTest {
public:
    static void SetWorkers(int32_t count) { ZCollectedHeap::heap()->_concurrent_gc_threads = count; }
};
class MutatorManagerTest {
public:
    static size_t RegistrySize(MutatorManager& manager) {
        std::lock_guard<std::mutex> lock(manager.runtimeMutatorRegistryMutex);
        return manager.runtimeMutators.size();
    }
};
class OopStorageTest {
public:
    static size_t BlockCount(const OopStorage& storage) {
        std::lock_guard<std::mutex> lock(storage.mutex);
        return storage.activeArray->blocks.size();
    }
    static size_t ConcurrentIterations(const OopStorage& storage) {
        std::lock_guard<std::mutex> lock(storage.mutex);
        return storage.concurrentIterationCount;
    }
};
}
