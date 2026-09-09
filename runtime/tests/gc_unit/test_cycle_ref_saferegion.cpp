// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(__linux__) && defined(MRT_GC_UNIT_TESTS)

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>

#include "Base/SysCall.h"
#include "Common/Runtime.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "gc_unittest.hpp"

// Access only changes C++ visibility in this test translation unit.  Both
// methods remain out-of-line symbols supplied by libcangjie-runtime.so.
#define private public
#include "Heap/WCollector/WCollector.h"
#undef private

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr auto kHandshakeLimit = std::chrono::seconds(5);

class CycleRefTestRuntime final : public Runtime {
public:
    explicit CycleRefTestRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        runtime = this;
    }

    ~CycleRefTestRuntime() override { runtime = nullptr; }

    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
};

bool WaitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + kHandshakeLimit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

GC_TEST(CycleRefSaferegion, ResolverParksBeforeCycleRootLock)
{
    MutatorManager manager;
    CycleRefTestRuntime runtime(manager);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    Mutator resolverMutator;
    resolverMutator.SetInSaferegion(Mutator::SAFE_REGION_TRUE);
    resolverMutator.SetSuspensionFlag(Mutator::SUSPENSION_FOR_SYNC);

    manager.SetSuspensionMutatorCount(1);

    std::atomic<bool> resolverReturned{ false };
    std::thread resolver([&] {
        ThreadLocal::SetMutator(&resolverMutator);
        collector.ResolveCycleRef();
        resolverReturned.store(true, std::memory_order_release);
        ThreadLocal::SetMutator(nullptr);
    });

    // SuspendForSync clears its request immediately before waiting on the
    // manager futex.  This is the deterministic receipt that the resolver has
    // reached the park, rather than a timing guess.
    const bool resolverParked = WaitUntil([&] {
        return !resolverMutator.HasSuspensionRequest(Mutator::SUSPENSION_FOR_SYNC) &&
            resolverMutator.InSaferegion();
    });

    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool consumerReturned = false;
    TracingCollector::RootSet roots;
    std::thread consumer([&] {
        collector.EnumAllSurrectedExportRoots(roots);
        {
            std::lock_guard<std::mutex> lock(completionMutex);
            consumerReturned = true;
        }
        completionCondition.notify_one();
    });

    bool consumerReturnedBeforeWake = false;
    {
        std::unique_lock<std::mutex> lock(completionMutex);
        consumerReturnedBeforeWake = completionCondition.wait_for(
            lock, kHandshakeLimit, [&] { return consumerReturned; });
    }

    // Always release both product threads before asserting, including the
    // deliberate broken-order arm where the consumer is waiting for the lock.
    manager.SetSuspensionMutatorCount(0);
    (void)Futex(manager.GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
    resolver.join();
    consumer.join();

    std::fprintf(stderr,
                 "CYCLE_REF_SAFEPOINT_TARGET_ASSERT reached parked=%d consumer_before_wake=%d "
                 "resolver_returned=%d roots=%zu\n",
                 resolverParked, consumerReturnedBeforeWake,
                 resolverReturned.load(std::memory_order_acquire), roots.size());

    // This is the target assertion for the deliberate-break arm: moving the
    // product cycle-lock owner before ScopedObjectAccess makes only this test
    // report consumer_before_wake=0.
    GC_EXPECT_TRUE(consumerReturnedBeforeWake);
    GC_EXPECT_TRUE(resolverParked);
    GC_EXPECT_TRUE(resolverReturned.load(std::memory_order_acquire));
    GC_EXPECT_EQ(roots.size(), 0u);
    GC_EXPECT_TRUE(resolverMutator.InSaferegion());
}

} // namespace

#endif // defined(__linux__) && defined(MRT_GC_UNIT_TESTS)
