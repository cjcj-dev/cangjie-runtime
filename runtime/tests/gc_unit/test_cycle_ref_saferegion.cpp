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
#include <vector>

#include "gc_heap_fixture.hpp"

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
#define protected public
#include "Heap/WCollector/WCollector.h"
#undef protected
#undef private
#include "Heap/Collector/CollectorProxy.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }
};
} // namespace MapleRuntime

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

struct HandlerSafepointContext {
    std::atomic<bool> entered{ false };
    std::atomic<bool> returned{ false };
    BaseObject* exportArg = nullptr;
    BaseObject* externArg = nullptr;
};

HandlerSafepointContext* handlerSafepointContext = nullptr;

void SafepointingCycleRefHandler(BaseObject* exportObj, BaseObject* externObj)
{
    HandlerSafepointContext* context = handlerSafepointContext;
    context->exportArg = exportObj;
    context->externArg = externObj;
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetSuspensionFlag(Mutator::SUSPENSION_FOR_SYNC);
    context->entered.store(true, std::memory_order_release);

    // This is the product late-safepoint entry used by the signal path. It
    // enters the saferegion and blocks in DoLeaveSaferegion until STW ends.
    HandleSafepoint(ThreadLocal::GetThreadLocalData());
    context->returned.store(true, std::memory_order_release);
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

GC_TEST(CycleRefSaferegion, CycleRootConsumerPublishesWorkStackRoots)
{
    MutatorManager manager;
    CycleRefTestRuntime runtime(manager);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());

    auto* exportRoot = reinterpret_cast<BaseObject*>(0x1000);
    auto* externRoot = reinterpret_cast<BaseObject*>(0x2000);
    collector.cycleRefWorkStack[exportRoot].push_back(externRoot);

    TracingCollector::RootSet roots;
    collector.EnumAllSurrectedExportRoots(roots);
    BaseObject* observedExtern = roots.empty() ? nullptr : roots.back().object();
    roots.pop_back();
    const bool ownerPresentAfterExtern = !roots.empty();
    BaseObject* observedExport = ownerPresentAfterExtern ? roots.back().object() : nullptr;
    if (ownerPresentAfterExtern) {
        roots.pop_back();
    }
    collector.cycleRefWorkStack.clear();

    std::fprintf(stderr,
                 "CYCLE_REF_CONSUMER_TARGET_ASSERT reached owner_after_extern=%d drained=%d\n",
                 ownerPresentAfterExtern, roots.empty());

    // The product consumer emits the externally referenced object after its
    // export owner.  These values are outputs of the real consumer, not values
    // manually fed to an assertion helper.
    GC_EXPECT_TRUE(ownerPresentAfterExtern);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(observedExtern), reinterpret_cast<uintptr_t>(externRoot));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(observedExport), reinterpret_cast<uintptr_t>(exportRoot));
    GC_EXPECT_TRUE(roots.empty());
}

GC_TEST(CycleRefSaferegion, HandlerSafepointKeepsCycleRootsConsumable)
{
    MutatorManager manager;
    CycleRefTestRuntime runtime(manager);
    GcHeapFixture fixture;
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), &collector);
    Mutator resolverMutator;
    resolverMutator.SetInSaferegion(Mutator::SAFE_REGION_TRUE);

    auto* exportRoot = reinterpret_cast<ExportObject*>(fixture.obj0);
    BaseObject* externRoot = fixture.obj1;
    const U64 exportHandle = Heap::GetHeap().RegisterExportRoot(exportRoot);
    const U32 exportId = ExportRootTable::ExportHandleIndex(exportHandle);
    *reinterpret_cast<U32*>(reinterpret_cast<uintptr_t>(exportRoot) + TYPEINFO_PTR_SIZE) = exportId;
    collector.cycleRefWorkStack[exportRoot].push_back(externRoot);

    HandlerSafepointContext context;
    handlerSafepointContext = &context;
    collector.SetCycleRefHandlerForTest(&SafepointingCycleRefHandler);
    manager.SetSuspensionMutatorCount(1);

    std::atomic<bool> resolverReturned{ false };
    std::thread resolver([&] {
        ThreadLocal::SetMutator(&resolverMutator);
        collector.ResolveCycleRef();
        resolverReturned.store(true, std::memory_order_release);
        ThreadLocal::SetMutator(nullptr);
    });

    const bool handlerParked = WaitUntil([&] {
        return context.entered.load(std::memory_order_acquire) &&
            !resolverMutator.HasSuspensionRequest(Mutator::SUSPENSION_FOR_SYNC) &&
            resolverMutator.InSaferegion();
    });

    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool consumerReturned = false;
    std::vector<BaseObject*> roots;
    std::thread consumer([&] {
        collector.VisitMinorValueRoots([&](BaseObject* object) { roots.push_back(object); });
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

    manager.SetSuspensionMutatorCount(0);
    (void)Futex(manager.GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
    resolver.join();
    consumer.join();

    BaseObject* observedExtern = roots.empty() ? nullptr : roots.back().object();
    if (!roots.empty()) {
        roots.pop_back();
    }
    BaseObject* observedExport = roots.empty() ? nullptr : roots.back().object();
    if (!roots.empty()) {
        roots.pop_back();
    }

    std::fprintf(stderr,
                 "CYCLE_REF_HANDLER_SAFEPOINT_TARGET_ASSERT reached parked=%d consumer_before_wake=%d "
                 "handler_returned=%d resolver_returned=%d roots_drained=%d\n",
                 handlerParked, consumerReturnedBeforeWake,
                 context.returned.load(std::memory_order_acquire),
                 resolverReturned.load(std::memory_order_acquire), roots.empty());

    collector.SetCycleRefHandlerForTest(nullptr);
    collector.cycleRefWorkStack.clear();
    handlerSafepointContext = nullptr;
    Heap::GetHeap().RemoveExportObject(exportHandle);
    RelocationReceiptTestAccess::BindCollector(Heap::GetHeap().GetCollectorResources(), nullptr);

    // Keep the target ordering assertion first: the deliberate lock-across-
    // handler cut must fail here, not at an earlier setup assertion.
    GC_EXPECT_TRUE(consumerReturnedBeforeWake);
    GC_EXPECT_TRUE(handlerParked);
    GC_EXPECT_TRUE(context.returned.load(std::memory_order_acquire));
    GC_EXPECT_TRUE(resolverReturned.load(std::memory_order_acquire));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(context.exportArg), reinterpret_cast<uintptr_t>(exportRoot));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(context.externArg), reinterpret_cast<uintptr_t>(externRoot));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(observedExport), reinterpret_cast<uintptr_t>(exportRoot));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(observedExtern), reinterpret_cast<uintptr_t>(externRoot));
    GC_EXPECT_TRUE(roots.empty());
}

} // namespace

#endif // defined(__linux__) && defined(MRT_GC_UNIT_TESTS)
