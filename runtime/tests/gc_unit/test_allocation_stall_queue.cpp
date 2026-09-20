// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDriver.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include <cstring>
#include "zunittest.hpp"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "CjScheduler.h"
extern "C" int CJ_ScheduleManagerInit();
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr auto kHangLimit = std::chrono::seconds(30);

// The production mutator/TLS callbacks are installed by Concurrency::Init.
class StallTestRuntime final : public Runtime {
public:
    StallTestRuntime()
    {
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        const ConcurrencyParam params = { 1024, 64, 1 };
        concurrency.Init(params);
    }
    ~StallTestRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
private:
    MutatorManager manager;
    Concurrency concurrency;
};

class OneUnitStallFixture {
private:
    StallTestRuntime runtime;
    ZPage* capacity{ nullptr };

public:
    RegionManager& manager;

    OneUnitStallFixture()
        : manager((ZStat::Initialize(), CreateStandaloneHeap(1), Heap::GetHeap().page_allocator()))
    {
        capacity = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, false);
    }

    void PublishCapacity()
    {
        ZPage* region = capacity;
        capacity = nullptr;
        manager.ReclaimRegion(region);
    }
};
} // namespace

GC_COMPONENT_OTHER_VM_TEST(AllocationStall, OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters)
{
    OneUnitStallFixture fixture;
    fixture.PublishCapacity();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* first = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, true, PageAge::eden, flags);
    ZPage* second = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, true, true, PageAge::eden, flags);
    const size_t count = (first != nullptr) + (second != nullptr);
    std::fprintf(stderr, "ALLOCATION_CAPACITY_TARGET count=%zu first=%p second=%p\n", count, first, second);
    GC_EXPECT_EQ(count, size_t{1});
}
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uint64_t> stallMarkSequence{0};
std::atomic<bool> stallReleaseMark{false};
std::atomic<bool> stallWindowTimeout{false};

void HoldStallMark()
{
    auto& heap = Heap::GetHeap();
    const auto snapshot = heap.GetCycleSnapshot(ZGenerationId::old);
    if (snapshot.phase != ZGenerationPhase::Mark) { return; }
    uint64_t empty = 0;
    if (!stallMarkSequence.compare_exchange_strong(empty, snapshot.sequence)) { return; }
    const auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!stallReleaseMark.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stallWindowTimeout = !stallReleaseMark.load();
}

void ObserveStallMark(const std::vector<BaseObject*>* objects)
{
    if (objects != nullptr) { HoldStallMark(); }
}
}

// zPageAllocator.cpp:2320-2362: a request arriving after old mark-start
// cannot be failed by that collection; the remaining queue drives another GC.
// Both waiters enter via Heap::alloc_page, not by constructing queue requests.
void RunProductStallWaiters(bool stopping = false)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(void*));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    U64 root;
    {
        ScopedObjectAccess access;
        root = heap.RegisterExportRoot(MObject::NewPinnedObject(type, 2 * sizeof(void*)));
    }
    stallMarkSequence = 0;
    stallReleaseMark = false;
    stallWindowTimeout = false;
    if (stopping) {
        // Hold the real driver owner before queuing allocation requests. Shutdown
        // publishes abort before this owner is released; no mark callback is needed.
        ZDriver::lock();
    } else {
        SetMarkClosureObserverForTest(ObserveStallMark);
    }
    const size_t bytes = heap.GetMaxCapacity(); // the rooted object excludes a whole-heap claim
    ZPage* results[2]{};
    uint64_t completedSequence[2]{};
    std::atomic<bool> done[2]{};
    std::atomic<Mutator*> waitingMutators[2]{};
    auto allocate = [&](size_t index) {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            waitingMutators[index].store(Mutator::GetMutator(), std::memory_order_release);
            results[index] = Heap::alloc_page(bytes, ZPageType::large);
            completedSequence[index] = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
        }
        done[index].store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    };
    std::thread first(allocate, 0);
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done[0].load() && std::chrono::steady_clock::now() < deadline) {
        Mutator* firstMutator = waitingMutators[0].load(std::memory_order_acquire);
        if (stopping ? (firstMutator != nullptr && firstMutator->InSaferegion()) :
                       stallMarkSequence.load() != 0) { break; }
        std::this_thread::yield();
    }
    const uint64_t firstMark = stopping ? heap.GetCycleSnapshot(ZGenerationId::old).sequence :
                                          stallMarkSequence.load();
    std::thread late(allocate, 1);
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (std::chrono::steady_clock::now() < deadline) {
        Mutator* a = waitingMutators[0].load(std::memory_order_acquire);
        Mutator* b = waitingMutators[1].load(std::memory_order_acquire);
        if (a != nullptr && b != nullptr && a->InSaferegion() && b->InSaferegion()) { break; }
        std::this_thread::yield();
    }
    Mutator* a = waitingMutators[0].load();
    Mutator* b = waitingMutators[1].load();
    const bool safe = a != nullptr && b != nullptr && a->InSaferegion() && b->InSaferegion() &&
                      !done[0].load() && !done[1].load();
    const bool stalling = heap.page_allocator().IsAllocationStalling();
    const size_t queued = (a != nullptr && a->InSaferegion() && !done[0].load()) +
                          (b != nullptr && b->InSaferegion() && !done[1].load());
    std::thread stopper;
    bool abortObserved = false;
    if (stopping) {
        stopper = std::thread([&] { heap.StopGCWork(); });
        deadline = std::chrono::steady_clock::now() + kHangLimit;
        while (!ZAbort::should_abort() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        abortObserved = ZAbort::should_abort();
        ZDriver::unlock();
    }
    stallReleaseMark.store(true, std::memory_order_release);
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while ((!done[0].load() || !done[1].load()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool completed = done[0].load() && done[1].load();
    std::fprintf(stderr, "STALL_COMPLETION_TARGET completed=%d first=%d late=%d pending=%zu\n",
                 completed, done[0].load(), done[1].load(), size_t{heap.page_allocator().IsAllocationStalling()});
    // A broken product notification must fail this invariant, rather than hang
    // in join and hide it behind the runner timeout. Stop this isolated VM on failure.
    if (!completed) {
        try { GC_EXPECT_TRUE(completed); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "STALL_COMPLETION_ASSERTION %s\n", error.what());
            std::_Exit(1);
        }
    }
    first.join();
    late.join();
    if (stopper.joinable()) { stopper.join(); }
    SetMarkClosureObserverForTest(nullptr);
    const bool retained = heap.GetExportObject(root) != nullptr;
    std::fprintf(stderr, "STALL_PRODUCT_LATE_TARGET queued=%zu first_mark=%llu first_done=%llu late_done=%llu "
                 "first_null=%d late_null=%d retained=%d timeout=%d\n", queued,
                 (unsigned long long)firstMark, (unsigned long long)completedSequence[0],
                 (unsigned long long)completedSequence[1], results[0] == nullptr, results[1] == nullptr,
                 retained, stallWindowTimeout.load());
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    const bool pending = heap.page_allocator().IsAllocationStalling();
    std::fprintf(stderr, "STALL_WAIT_TARGET safe=%d stalling=%d completed=%d pending=%d\n",
                 safe, stalling, completed, pending);
    GC_EXPECT_TRUE(safe && stalling);
    GC_EXPECT_FALSE(pending);
    GC_EXPECT_EQ(queued, size_t{2});
    GC_EXPECT_FALSE(stallWindowTimeout.load());
    if (!stopping) { GC_EXPECT_TRUE(firstMark != 0); }
    GC_EXPECT_TRUE(results[0] == nullptr && results[1] == nullptr && retained);
    if (stopping) {
        std::fprintf(stderr, "STALL_SHUTDOWN_TARGET abort_observed=%d abort_retained=%d late_done=%llu first_mark=%llu\n",
                     abortObserved, ZAbort::should_abort(), (unsigned long long)completedSequence[1],
                     (unsigned long long)firstMark);
        GC_EXPECT_TRUE(abortObserved && ZAbort::should_abort());
        GC_EXPECT_EQ(completedSequence[1], firstMark);
    } else {
        GC_EXPECT_TRUE(completedSequence[1] > firstMark);
    }
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(AllocationStall, ProductLateWaiterRequiresNextCollection) { RunProductStallWaiters(); }
GC_RUNTIME_OTHER_VM_TEST(AllocationStall, ProductShutdownAnswersPendingWaiters) { RunProductStallWaiters(true); }

// ZGC zPageAllocator.cpp:2167-2189: reclaimed capacity is claimed exclusively
// before the allocator wakes a waiter. Both requests arrive during old marking.
GC_RUNTIME_OTHER_VM_TEST(AllocationStall, ProductReturnedCapacityServesOnlyOneWaiter)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    constexpr size_t occupiedBytes = 48 * 1024 * 1024;
    constexpr size_t requestedBytes = 40 * 1024 * 1024;
    alignas(TypeInfo) unsigned char occupiedStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char requestStorage[sizeof(TypeInfo)]{};
    auto makeType = [&](unsigned char* storage, size_t bytes) {
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(bytes - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(TypeInfo));
        return type;
    };
    TypeInfo* occupiedType = makeType(occupiedStorage, 2 * sizeof(void*));
    TypeInfo* requestType = makeType(requestStorage, requestedBytes);
    // Large movable objects use the product object allocator; the pinned-page
    // API is limited to its fixed-size page and cannot represent these sizes.
    auto allocateObject = [&](TypeInfo* type, size_t bytes) -> BaseObject* {
        const uintptr_t address = heap.object_allocator().alloc(bytes, PageAge::eden, false);
        if (address == 0) { return nullptr; }
        auto* object = reinterpret_cast<BaseObject*>(address);
        object->SetClassInfo(type);
        return object;
    };
    U64 occupiedRoot;
    ZPage* capacity = nullptr;
    {
        ScopedObjectAccess access;
        occupiedRoot = heap.RegisterExportRoot(MObject::NewPinnedObject(occupiedType, 2 * sizeof(void*)));
        // Reserve a real page without publishing an object on it. Returning
        // that page later is the ordinary product capacity-supply entry.
        capacity = Heap::alloc_page(occupiedBytes, ZPageType::large);
        GC_EXPECT_TRUE(capacity != nullptr);
    }
    stallMarkSequence = 0;
    stallReleaseMark = false;
    stallWindowTimeout = false;
    SetMarkClosureObserverForTest(ObserveStallMark);
    // Enter through the public collection request, not a hand-assembled driver cycle.
    std::thread collection([&] {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        heap.RequestGC(GC_REASON_USER, false);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (stallMarkSequence.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    BaseObject* results[2]{};
    U64 resultRoots[2]{};
    std::atomic<bool> done[2]{};
    std::atomic<Mutator*> waitingMutators[2]{};
    auto allocate = [&](size_t index) {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            waitingMutators[index].store(Mutator::GetMutator(), std::memory_order_release);
            results[index] = allocateObject(requestType, requestedBytes);
            if (results[index] != nullptr) { resultRoots[index] = heap.RegisterExportRoot(results[index]); }
        }
        done[index].store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    };
    std::thread first(allocate, 0), second(allocate, 1);
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    auto waiting = [&] {
        size_t count = 0;
        for (size_t i = 0; i < 2; ++i) {
            Mutator* mutator = waitingMutators[i].load(std::memory_order_acquire);
            if (mutator != nullptr && mutator->InSaferegion() && !done[i].load()) { ++count; }
        }
        return count;
    };
    while (waiting() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const size_t queued = waiting();
    Heap::free_page(capacity);
    ZAllocationFlags nonBlocking;
    nonBlocking.set_non_blocking();
    ZPage* competing = Heap::alloc_page(requestedBytes, ZPageType::large, false, true, true,
                                      PageAge::eden, nonBlocking);
    stallReleaseMark.store(true, std::memory_order_release);
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while ((!done[0].load() || !done[1].load()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool completed = done[0].load() && done[1].load();
    std::fprintf(stderr, "STALL_CAPACITY_COMPLETION_TARGET completed=%d queued=%zu\n", completed, queued);
    if (!completed) {
        try { GC_EXPECT_TRUE(completed); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "STALL_CAPACITY_COMPLETION_ASSERTION %s\n", error.what());
            std::_Exit(1);
        }
    }
    first.join();
    second.join();
    collection.join();
    SetMarkClosureObserverForTest(nullptr);
    const size_t successes = (results[0] != nullptr) + (results[1] != nullptr);
    // zPageAllocator.cpp:2167-2189: allocation return values are the consumer
    // result of satisfy/fail; there is no diagnostic event ledger in ZGC.
    const size_t failed = (results[0] == nullptr) + (results[1] == nullptr);
    const bool pending = heap.page_allocator().IsAllocationStalling();
    std::fprintf(stderr, "STALL_CAPACITY_TARGET successes=%zu failed=%zu pending=%d first=%p second=%p competing=%p\n",
                 successes, failed, pending, results[0], results[1], competing);
    for (size_t i = 0; i < 2; ++i) {
        if (results[i] != nullptr) { heap.RemoveExportObject(resultRoots[i]); }
    }
    heap.RemoveExportObject(occupiedRoot);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(competing == nullptr);
    GC_EXPECT_EQ(successes, size_t{1});
    GC_EXPECT_EQ(failed, size_t{1});
    GC_EXPECT_FALSE(pending);
    GC_EXPECT_EQ(queued, size_t{2});
    GC_EXPECT_FALSE(stallWindowTimeout.load());
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif
