#include "gc_allocation_flags.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fstream>
#include <iterator>
#include <unistd.h>

#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/concurrentGCBreakpoints.hpp"
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
#include "Mutator/Handshake.h"
#include "Heap/z/zDriverPort.hpp"

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
        capacity = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::eden, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
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
    ZPage* first = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::eden, flags);
    ZPage* second = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::eden, flags);
    const size_t count = (first != nullptr) + (second != nullptr);
    std::fprintf(stderr, "ALLOCATION_CAPACITY_TARGET count=%zu first=%p second=%p\n", count, first, second);
    GC_EXPECT_EQ(count, size_t{1});
}
#if defined(MRT_TESTABLE_INTERNALS)
// A single allocation enters after the major's young prelude. The non-stop
// debugger may park the minor driver before its lock to deterministically let
// old completion consume this queue first. No product state is injected.
GC_RUNTIME_OTHER_VM_TEST(RequestWorkers, StallAfterYoungPrelude)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 4;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 3;
    params.gcParam.staticGCThreads = true;
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
    ConcurrentGCBreakpoints::AcquireControl();
    const bool stopped = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    std::atomic<bool> done{false};
    ZPage* result = nullptr;
    std::thread waiter([&] {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            result = Heap::alloc_page(heap.GetMaxCapacity(), ZPageType::large);
        }
        done.store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!heap.page_allocator().IsAllocationStalling() && !done.load() &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    const bool queued = heap.page_allocator().IsAllocationStalling();
    ConcurrentGCBreakpoints::ReleaseControl();
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    std::fprintf(stderr, "REQUEST_STALL_COMPLETION_TARGET stopped=%d queued=%d done=%d\n",
                 stopped, queued, done.load());
    if (!done.load()) { std::_Exit(1); }
    waiter.join();
    GC_EXPECT_TRUE(stopped && queued);
    GC_EXPECT_TRUE(result == nullptr);
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
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
    if (stopping) {
        // Hold the real driver owner before queuing allocation requests. Shutdown
        // publishes abort before this owner is released; no mark callback is needed.
        ZDriver::lock();
    } else {
        ConcurrentGCBreakpoints::AcquireControl();
    }
    const size_t bytes = heap.GetMaxCapacity(); // the rooted object excludes a whole-heap claim
    ZPage* results[2]{};
    uint64_t completedSequence[2]{};
    std::atomic<bool> done[2]{};
    std::atomic<Mutator*> waitingMutators[2]{};
    std::atomic<bool> releaseMutators{false};
    auto allocate = [&](size_t index) {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            waitingMutators[index].store(Mutator::GetMutator(), std::memory_order_release);
            results[index] = Heap::alloc_page(bytes, ZPageType::large);
            completedSequence[index] = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
        }
        done[index].store(true, std::memory_order_release);
        // The controller reads the published mutator state until both results
        // are collected. Keep its lifetime valid even on an early return.
        while (!releaseMutators.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    };
    std::thread first(allocate, 0);
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done[0].load() && std::chrono::steady_clock::now() < deadline) {
        Mutator* firstMutator = waitingMutators[0].load(std::memory_order_acquire);
        if (firstMutator != nullptr && firstMutator->InSaferegion()) { break; }
        std::this_thread::yield();
    }
    // Existing ZGC breakpoint stops the real old mark-start, before the late
    // request snapshots its sequence. No diagnostic callback supplies the input.
    const bool markStopped = stopping || ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    const uint64_t firstMark = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
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
    if (!stopping) { ConcurrentGCBreakpoints::ReleaseControl(); }
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
    releaseMutators.store(true, std::memory_order_release);
    first.join();
    late.join();
    if (stopper.joinable()) { stopper.join(); }
    const bool retained = heap.GetExportObject(root) != nullptr;
    std::fprintf(stderr, "STALL_PRODUCT_LATE_TARGET queued=%zu first_mark=%llu first_done=%llu late_done=%llu "
                 "first_null=%d late_null=%d retained=%d mark_stopped=%d\n", queued,
                 (unsigned long long)firstMark, (unsigned long long)completedSequence[0],
                 (unsigned long long)completedSequence[1], results[0] == nullptr, results[1] == nullptr,
                 retained, markStopped);
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    const bool pending = heap.page_allocator().IsAllocationStalling();
    std::fprintf(stderr, "STALL_WAIT_TARGET safe=%d stalling=%d completed=%d pending=%d\n",
                 safe, stalling, completed, pending);
    GC_EXPECT_TRUE(safe && stalling);
    GC_EXPECT_FALSE(pending);
    GC_EXPECT_EQ(queued, size_t{2});
    GC_EXPECT_TRUE(markStopped);
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
    TypeInfo* occupiedType = makeType(occupiedStorage, occupiedBytes);
    TypeInfo* requestType = makeType(requestStorage, requestedBytes);
    // Large movable objects use the product object allocator; the pinned-page
    // API is limited to its fixed-size page and cannot represent these sizes.
    auto allocateObject = [&](TypeInfo* type, size_t bytes) -> BaseObject* {
        const uintptr_t address = heap.object_allocator().alloc(bytes);
        if (address == 0) { return nullptr; }
        auto* object = reinterpret_cast<BaseObject*>(address);
        object->SetClassInfo(type);
        return object;
    };
    U64 occupiedRoot;
    // RunTo requests the real driver collection and stops its old marking entry.
    ConcurrentGCBreakpoints::AcquireControl();
    const bool markStopped = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED");
    // Old concurrent marking releases the driver lock, so its breakpoint
    // does not stop young collections (ZGC zGeneration.cpp:995). Hold the
    // existing driver owner until both requests are queued. Capacity is a
    // rooted product object, never a raw page retained across a collection.
    ZDriver::lock();
    {
        ScopedObjectAccess access;
        BaseObject* occupied = allocateObject(occupiedType, occupiedBytes);
        GC_EXPECT_TRUE(occupied != nullptr);
        occupiedRoot = heap.RegisterExportRoot(occupied);
    }
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    BaseObject* results[2]{};
    U64 resultRoots[2]{};
    std::atomic<bool> done[2]{};
    std::atomic<Mutator*> waitingMutators[2]{};
    std::atomic<bool> releaseMutators{false};
    auto allocate = [&](size_t index) {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            waitingMutators[index].store(Mutator::GetMutator(), std::memory_order_release);
            results[index] = allocateObject(requestType, requestedBytes);
            if (results[index] != nullptr) { resultRoots[index] = heap.RegisterExportRoot(results[index]); }
        }
        done[index].store(true, std::memory_order_release);
        // The controller reads the published mutator state until both results
        // are collected. Keep its lifetime valid even on an early return.
        while (!releaseMutators.load(std::memory_order_acquire)) { std::this_thread::yield(); }
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
    // Removing the root makes the next minor collection reclaim this object
    // through select_relocation_set -> free_empty_pages -> free_page (ZGC
    // zGeneration.cpp:220-240), which reserves returned capacity for a waiter.
    heap.RemoveExportObject(occupiedRoot);
    ZDriver::unlock();
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done[0].load() && !done[1].load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ZAllocationFlags nonBlocking;
    nonBlocking.set_non_blocking();
    ZPage* competing = Heap::alloc_page(requestedBytes, ZPageType::large, false, PageAge::eden, nonBlocking);
    ConcurrentGCBreakpoints::ReleaseControl();
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
    releaseMutators.store(true, std::memory_order_release);
    first.join();
    second.join();
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
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(competing == nullptr);
    GC_EXPECT_EQ(successes, size_t{1});
    GC_EXPECT_EQ(failed, size_t{1});
    GC_EXPECT_FALSE(pending);
    GC_EXPECT_EQ(queued, size_t{2});
    GC_EXPECT_TRUE(markStopped);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

// ZGC zDirector.cpp:337 and zPageAllocator.cpp:2308. Both inputs come from
// actual blocked Heap::alloc_page requests. The existing old-mark breakpoint
// keeps their old epoch stable while the real director thread evaluates rules.
namespace {
void CheckDirectorStallGate(bool waitingForOld)
{
    const std::string logPath = std::string("./director-stall-") + std::to_string(getpid()) + ".log";
    GC_EXPECT_EQ(setenv("MRT_REPORT", logPath.c_str(), 1), 0);
    // Logger::GetLogPath appends the process id to each report path.
    const std::string actualLogPath = logPath + "." + std::to_string(getpid());
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
    ConcurrentGCBreakpoints::AcquireControl();
    // Before old mark-start: request later sees old, so it must NOT suppress.
    // After old mark-start: its automatic minor advances only young, so it MUST suppress.
    bool markStopped = true;
    if (waitingForOld) { markStopped = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED"); }
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            Heap::alloc_page(heap.GetMaxCapacity(), ZPageType::large);
        }
        done.store(true, std::memory_order_release);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    auto deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!heap.page_allocator().IsAllocationStalling() && !done.load() &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    if (!waitingForOld) { markStopped = ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED"); }
    const std::string expected = waitingForOld
        ? "Rule Minor: Allocation Stall, StallingForOld: 1, Stalling: 1, Suppressed: 1"
        : "Rule Minor: Allocation Stall, StallingForOld: 0, Stalling: 1, Suppressed: 0";
    bool observed = false;
    bool entered = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        std::ifstream input(actualLogPath);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        entered = text.find("Stalling: 1, Suppressed:") != std::string::npos;
        observed = text.find(expected) != std::string::npos;
        if (!observed) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    } while (!observed && std::chrono::steady_clock::now() < deadline);
    const bool stalling = heap.page_allocator().IsAllocationStalling();
    ConcurrentGCBreakpoints::ReleaseControl();
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    if (!done.load()) {
        std::fprintf(stderr, "DIRECTOR_STALL_CLEANUP_TIMEOUT log=%s\n", logPath.c_str());
        std::_Exit(1);
    }
    waiter.join();
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    std::fprintf(stderr, "DIRECTOR_STALL_TARGET waiting_for_old=%d observed=%d entered=%d stalled=%d "
        "mark_stopped=%d log=%s\n", waitingForOld, observed, entered, stalling, markStopped, actualLogPath.c_str());
    // Target assertion comes first: no existence assertion can hide its failure.
    GC_EXPECT_TRUE(observed);
    GC_EXPECT_TRUE(entered && stalling && markStopped);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(GcDirector, OldStallSuppressesMinorAllocationRate)
{
    CheckDirectorStallGate(true);
}
GC_RUNTIME_OTHER_VM_TEST(GcDirector, OtherStallDoesNotSuppressMinorAllocationRate)
{
    CheckDirectorStallGate(false);
}

namespace {
class FutureWaitHandshake final : public HandshakeClosure {
public:
    FutureWaitHandshake() : HandshakeClosure("future wait") {}
    void do_thread(ThreadLocalData* tls) override
    {
        counted = tls->mutator != nullptr && tls->mutator->InSaferegion() &&
                  Handshake::ForTls(tls)->observed_safe();
    }
    bool counted = false;
};

void CheckProductFutureWait(bool allocation)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    // Hold the real driver before it can finish the request and notify its future.
    ZDriver::lock();
    std::atomic<ThreadLocalData*> waiting{nullptr};
    std::atomic<bool> done{false};
    std::atomic<bool> release{false};
    bool restored = false;
    std::thread waiter([&] {
        manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        {
            ScopedObjectAccess access;
            waiting.store(ThreadLocal::GetThreadLocalData(), std::memory_order_release);
            if (allocation) {
                (void)Heap::alloc_page(heap.GetMaxCapacity() + ZGranuleSize, ZPageType::large);
            } else {
                heap.RequestGC(GC_REASON_USER);
            }
            restored = !Mutator::GetMutator()->InSaferegion() && !Handshake::Current().observed_safe();
        }
        done.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    ThreadLocalData* tls = nullptr;
    bool safe = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        tls = waiting.load(std::memory_order_acquire);
        safe = tls != nullptr && tls->mutator->InSaferegion() && Handshake::ForTls(tls)->observed_safe() &&
               !done.load(std::memory_order_acquire);
        if (safe || done.load()) { break; }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    FutureWaitHandshake operation;
    if (safe) { Handshake::execute(&operation, tls); }
    const bool queued = !allocation || heap.page_allocator().IsAllocationStalling();
    std::fprintf(stderr, "FUTURE_HANDSHAKE_TARGET allocation=%d safe=%d counted=%d queued=%d done=%d\n",
                 allocation, safe, operation.counted, queued, done.load());
    // The product safe-state and actual handshake result enter this assertion.
    // Exit this isolated VM before a broken waiter can prevent GC cleanup.
    try { GC_EXPECT_TRUE(safe && operation.counted && queued && !done.load()); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FUTURE_HANDSHAKE_ASSERTION %s\n", error.what());
        std::_Exit(1);
    }
    ZDriver::unlock();
    deadline = std::chrono::steady_clock::now() + kHangLimit;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    if (!done.load()) {
        std::fprintf(stderr, "FUTURE_COMPLETION_TARGET done=0\n");
        std::_Exit(1);
    }
    release.store(true, std::memory_order_release);
    waiter.join();
    std::fprintf(stderr, "FUTURE_RESTORE_TARGET restored=%d\n", restored);
    GC_EXPECT_TRUE(restored);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
} // namespace

GC_RUNTIME_OTHER_VM_TEST(FutureWait966, StalledMutatorParticipatesInHandshake)
{
    CheckProductFutureWait(true);
}

GC_RUNTIME_OTHER_VM_TEST(FutureWait966, CollectionWaitParticipatesInHandshake)
{
    CheckProductFutureWait(false);
}

GC_OTHER_VM_TEST(FutureWait966, GCThreadPreservesHandshakeState)
{
    ZDriverPort port;
    std::atomic<HandshakeState*> state{nullptr};
    std::atomic<bool> done{false};
    std::atomic<bool> release{false};
    bool unchanged = false;
    std::thread gc([&] {
        ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
        Handshake::Current().leave_safe();
        state.store(&Handshake::Current(), std::memory_order_release);
        port.send_sync(ZDriverRequest(GC_REASON_USER, 1, 1));
        unchanged = !Handshake::Current().observed_safe() && ThreadLocal::GetMutator() == nullptr;
        done.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        Handshake::Current().enter_safe();
    });
    const ZDriverRequest request = port.receive();
    const bool blockedUnchanged = state.load() != nullptr && !state.load()->observed_safe() && !done.load();
    port.ack();
    while (!done.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    release.store(true, std::memory_order_release);
    gc.join();
    std::fprintf(stderr, "FUTURE_GC_CONTROL_TARGET blocked_unchanged=%d returned_unchanged=%d cause=%d\n",
                 blockedUnchanged, unchanged, static_cast<int>(request.cause()));
    GC_EXPECT_TRUE(blockedUnchanged && unchanged);
    GC_EXPECT_EQ(request.cause(), GC_REASON_USER);
}
