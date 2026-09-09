// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "gc_unittest.hpp"
#include "Heap/Heap.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct CycleRefWorkTestAccess {
    static std::unique_lock<std::mutex> HoldOwner(WCollector& collector)
    {
        return std::unique_lock<std::mutex>(collector.cycleWorkStackMtx);
    }

    static void SeedCycleWork(WCollector& collector, BaseObject* key)
    {
        collector.cycleRefWorkStack.emplace(key, std::list<BaseObject*>{});
    }

    static void SeedDiscoveredWork(WCollector& collector, BaseObject* key)
    {
        collector.discoveredExternObjects.emplace(key, std::list<BaseObject*>{});
    }

    static bool HasCycleRefWork(WCollector& collector)
    {
        return collector.HasCycleRefWork();
    }

    static bool ContainsCycleWork(WCollector& collector, BaseObject* key)
    {
        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
        return collector.cycleRefWorkStack.find(key) != collector.cycleRefWorkStack.end();
    }

    static bool DiscoveredWorkEmpty(WCollector& collector)
    {
        return collector.discoveredExternObjects.empty();
    }
};

} // namespace MapleRuntime

namespace {

struct CallState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool completed = false;
    bool result = false;
};

struct CallResult {
    bool result = false;
};

template<typename ProductCall>
CallResult RunWhileOwnerHeld(WCollector& collector, ProductCall&& call, const char* arm)
{
    auto owner = CycleRefWorkTestAccess::HoldOwner(collector);
    CallState state;
    std::thread worker([&]() {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.entered = true;
        }
        state.changed.notify_all();
        const bool result = call();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.result = result;
            state.completed = true;
        }
        state.changed.notify_all();
    });
    JoinGuard workerGuard(worker);

    bool entered = false;
    bool completedWhileHeld = false;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        entered = state.changed.wait_for(lock, std::chrono::seconds(2), [&]() { return state.entered; });
        if (entered) {
            completedWhileHeld = state.changed.wait_for(
                lock, std::chrono::milliseconds(100), [&]() { return state.completed; });
        }
    }

    owner.unlock();
    bool completedAfterRelease = false;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        completedAfterRelease = state.changed.wait_for(
            lock, std::chrono::seconds(2), [&]() { return state.completed; });
    }
    worker.join();

    std::fprintf(stderr,
                 "CYCLE_REF_LOCK_ASSERT arm=%s entered=%d completed_while_held=%d "
                 "completed_after_release=%d result=%d\n",
                 arm, entered ? 1 : 0, completedWhileHeld ? 1 : 0,
                 completedAfterRelease ? 1 : 0, state.result ? 1 : 0);
    GC_EXPECT_TRUE(entered);
    GC_EXPECT_FALSE(completedWhileHeld);
    GC_EXPECT_TRUE(completedAfterRelease);
    return CallResult{ state.result };
}

} // namespace

GC_TEST(CycleRefWorkLock, ProductConsumerBlocksOnOwnerAndReturnsSnapshot)
{
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    auto* key = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    {
        auto owner = CycleRefWorkTestAccess::HoldOwner(collector);
        CycleRefWorkTestAccess::SeedCycleWork(collector, key);
    }

    CallResult state = RunWhileOwnerHeld(
        collector, [&]() { return CycleRefWorkTestAccess::HasCycleRefWork(collector); }, "consumer");
    GC_EXPECT_TRUE(state.result);
    std::fprintf(stderr, "CYCLE_REF_RESULT_ASSERT arm=consumer pending=1\n");
}

GC_TEST(CycleRefWorkLock, ProductProducerBlocksOnSameOwnerAndMovesWork)
{
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    auto* key = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    CycleRefWorkTestAccess::SeedDiscoveredWork(collector, key);

    CallResult state = RunWhileOwnerHeld(collector, [&]() {
        collector.PrepareCycleRef();
        return true;
    }, "producer");
    GC_EXPECT_TRUE(state.result);
    GC_EXPECT_TRUE(CycleRefWorkTestAccess::ContainsCycleWork(collector, key));
    GC_EXPECT_TRUE(CycleRefWorkTestAccess::DiscoveredWorkEmpty(collector));
    std::fprintf(stderr, "CYCLE_REF_RESULT_ASSERT arm=producer moved=1 discovered_empty=1\n");
}
