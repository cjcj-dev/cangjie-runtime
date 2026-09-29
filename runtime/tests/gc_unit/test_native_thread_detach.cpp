// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "TypeInfoManager.h"
#include "ObjectModel/MObject.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

using namespace MapleRuntime;
namespace MapleRuntime { extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize); }

// Test scheduling state for the external non-stop debugger. It never changes
// product state or replaces a product call. Without the debugger these tests
// run the same natural thread return and real collector entry.
extern "C" {
std::atomic<int> native1286_allow_exit{1};
ThreadGCData* native1286_data = nullptr;
BaseObject* native1286_object = nullptr;
__attribute__((noinline)) void native1286_ready() { std::fprintf(stderr, "NATIVE1286_READY\n"); }
}
namespace {
template<class F> bool WaitNative(F f)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!f() && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
    return f();
}
void InitNativeRuntime()
{
    RuntimeParam p{};
    p.heapParam.heapSize = 128 * 1024;
    p.coParam.processorNum = 1;
    p.gcParam.concGCThreads = 2;
    p.gcParam.concGCThreadsSet = true;
    p.gcParam.youngGCThreads = 1;
    p.gcParam.youngGCThreadsSet = true;
    p.gcParam.oldGCThreads = 1;
    p.gcParam.oldGCThreadsSet = true;
    p.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&p), E_OK);
}
void* AllocateNativeTarget(void*)
{
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(32 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    {
        HandleMark handles(*mutator);
        Handle target(mutator, MCC_NewObject(type, 32));
        Heap::GetHeap().RequestGC(GC_REASON_USER);
        native1286_object = target();
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<bool> nativePushObserved{false};
void ObserveNativePush()
{
    if (&ThreadLocal::GetGCData() == native1286_data) {
        nativePushObserved.store(true, std::memory_order_release);
        std::fprintf(stderr, "NATIVE1286_PUSH_CREATED owner=%p native=%p managed=%d\n",
            static_cast<void*>(native1286_data), static_cast<void*>(ThreadLocal::GetThreadLocalData()->nativeGCData),
            native1286_data->managedOwner);
    }
}
#endif
bool ContainsNative(const ThreadGCData* target)
{
    for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) {
        if (&it.Current()->nativeData == target) { return true; }
    }
    return false;
}
}

namespace {
void CheckNativeFinalPublication(bool detach)
{
    InitNativeRuntime();
    auto task = RunCJTask(AllocateNativeTarget, nullptr);
    GC_EXPECT_TRUE(task != nullptr);
    void* ret = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &ret), E_OK);
    ReleaseHandle(task);
    ConcurrentGCBreakpoints::AcquireControl();
    const bool reached = ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
    auto* page = Heap::page(reinterpret_cast<uintptr_t>(native1286_object));
    const bool old = page != nullptr && !page->IsYoungRegion();
    const bool before = old && page->is_object_marked(from_object(native1286_object), false);
    const bool debugger = std::getenv("GC_NATIVE_GDB") != nullptr;
    native1286_allow_exit.store(debugger || !detach ? 0 : 1);
    std::atomic<bool> produced{false};
    size_t privateEntries = 0;
#if defined(MRT_TESTABLE_INTERNALS)
    nativePushObserved.store(false);
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(ObserveNativePush);
#endif
    std::thread producer([&] {
        ThreadLocal::InitializeCleaner();
        native1286_data = &ThreadLocal::GetGCData();
        // Real native-store slow path -> Heap::MarkObjectIfActive -> ZMark.
        ZBarrier::native_store_slow_path(from_object(native1286_object));
        privateEntries = native1286_data->markStacks[1].Population();
        produced.store(true, std::memory_order_release);
        while (!native1286_allow_exit.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        // The only detach is the product's TLS destructor after this return.
    });
    const bool ready = WaitNative([&] { return produced.load(std::memory_order_acquire); });
    native1286_ready();
    if (!debugger && detach) { producer.join(); }
    const bool afterMark = ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED");
    if (debugger) { producer.join(); }
    page = Heap::page(reinterpret_cast<uintptr_t>(native1286_object));
    const bool marked = page != nullptr && page->is_object_marked(from_object(native1286_object), false);
    if (!detach) {
        native1286_allow_exit.store(1, std::memory_order_release);
        producer.join();
    }
    const bool removed = !ContainsNative(native1286_data);
    std::fprintf(stderr, "NATIVE_FINAL_PUBLICATION executed=1 detach=%d reached=%d old=%d before=%d ready=%d private=%zu marked=%d removed=%d after_mark=%d\n",
                 detach, reached, old, before, ready, privateEntries, marked, removed, afterMark);
    // A disconnected publication can leave work beyond this cycle's mark end.
    // Assert the observed result before cleanup can conceal that failure.
    if (!(marked && removed)) {
        std::fprintf(stderr, "NATIVE_FINAL_PUBLICATION_ASSERT_FAIL marked=%d removed=%d\n", marked, removed);
        std::fflush(stderr);
        _exit(1);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(nullptr);
    const bool push = nativePushObserved.load();
    std::fprintf(stderr, "NATIVE_PUSH_WITNESS observed=%d\n", push);
#endif
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    // Evaluate the product outcome first, before setup assertions.
    GC_EXPECT_TRUE(marked && removed);
    GC_EXPECT_TRUE(reached && old && !before && ready && privateEntries == 1 && afterMark);
#if defined(MRT_TESTABLE_INTERNALS)
    GC_EXPECT_TRUE(push);
#endif
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

}
GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, FinalPublication) { CheckNativeFinalPublication(true); }
GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, AttachedPublication) { CheckNativeFinalPublication(false); }

GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, IteratorGracePeriod)
{
    InitNativeRuntime();
    std::atomic<ThreadGCData*> data{nullptr};
    std::atomic<pid_t> tid{0};
    std::atomic<bool> finish{false}, joined{false}, lateReady{false}, lateRelease{false};
    std::thread owner([&] {
        ThreadLocal::InitializeCleaner();
        tid.store(static_cast<pid_t>(syscall(SYS_gettid)));
        data.store(&ThreadLocal::GetGCData(), std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    GC_EXPECT_TRUE(WaitNative([&] { return data.load() != nullptr; }));
    std::thread joiner([&] { owner.join(); joined.store(true, std::memory_order_release); });
    bool found = false, removed = false, waiting = false, retained = false, pauseDuringGrace = false;
    std::thread late;
    {
        CleanThreadLocalData::Iterator first;
        while (!first.End() && &first.Current()->nativeData != data.load()) { first.Step(); }
        found = !first.End();
        const auto masks = found ? first.Current()->nativeData.storeGoodMask : 0;
        finish.store(true, std::memory_order_release);
        removed = WaitNative([&] { return !ContainsNative(data.load()); });
        waiting = WaitNative([&] {
            if (joined.load()) { return true; }
            std::ifstream input("/proc/self/task/" + std::to_string(tid.load()) + "/wchan");
            std::string where; input >> where;
            return where.find("futex") != std::string::npos;
        });
        retained = found && !joined.load() && first.Current()->nativeData.storeGoodMask == masks && masks != 0;
        // A reader may request a real pause while the remover waits. Native
        // removal must already have left STS or this forms a wait cycle.
        {
            ScopedStopTheWorld pause("native owner grace period");
            pauseDuringGrace = MutatorManager::Instance().WorldStopped();
        }
        // Enter after the first remover has begun its grace period. This
        // second generation must not delay the first generation's completion.
        late = std::thread([&] {
            CleanThreadLocalData::Iterator second;
            lateReady.store(true, std::memory_order_release);
            while (!lateRelease.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        });
        (void)WaitNative([&] { return lateReady.load(); });
    }
    const bool progressed = WaitNative([&] { return joined.load(std::memory_order_acquire); });
    std::fprintf(stderr, "NATIVE_ITERATOR_GRACE executed=1 found=%d unlinked=%d waiting=%d retained=%d later_reader=%d completed=%d pause_during_grace=%d\n",
                 found, removed, waiting, retained, lateReady.load(), progressed, pauseDuringGrace);
    lateRelease.store(true, std::memory_order_release);
    late.join();
    joiner.join();
    GC_EXPECT_TRUE(found && removed && waiting && retained && progressed && pauseDuringGrace);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

extern "C" {
std::atomic<int> native1286_shutdown_release{1};
__attribute__((noinline)) void native1286_shutdown_ready() { std::fprintf(stderr, "NATIVE1286_SHUTDOWN_READY\n"); }
}
GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, ShutdownUnlinksLateNative)
{
    InitNativeRuntime();
    std::atomic<ThreadGCData*> lateData{nullptr};
    std::atomic<bool> exitLate{false};
    std::thread late([&] {
        ThreadLocal::InitializeCleaner();
        lateData.store(&ThreadLocal::GetGCData(), std::memory_order_release);
        while (!exitLate.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    const bool attached = WaitNative([&] { return lateData.load() != nullptr; });
    ConcurrentGCBreakpoints::AcquireControl();
    const bool active = ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
    native1286_shutdown_release.store(std::getenv("GC_NATIVE_GDB") ? 0 : 1);
    std::thread release([&] {
        while (!native1286_shutdown_release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        ConcurrentGCBreakpoints::ReleaseControl();
    });
    native1286_shutdown_ready();
    const int fini = FiniCJRuntime();
    release.join();
    const bool beforeExit = ContainsNative(lateData.load());
    exitLate.store(true, std::memory_order_release);
    late.join();
    const bool removed = !ContainsNative(lateData.load());
    bool nativeOnly = true;
    for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) {
        nativeOnly &= !it.Current()->nativeData.managedOwner;
    }
    std::fprintf(stderr, "NATIVE_SHUTDOWN_LIFETIME executed=1 active=%d attached=%d fini=%d before_exit=%d removed=%d native_only=%d\n",
                 active, attached, fini, beforeExit, removed, nativeOnly);
    GC_EXPECT_TRUE(beforeExit && removed && nativeOnly);
    GC_EXPECT_TRUE(active && attached && fini == E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, StableCarrierAndLogicalInventory)
{
    InitNativeRuntime();
    std::atomic<bool> ready{false}, release{false};
    ThreadGCData* native = nullptr;
    Mutator* logical = nullptr;
    std::thread carrier([&] {
        auto& manager = MutatorManager::Instance();
        logical = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        native = ThreadLocal::GetThreadLocalData()->nativeGCData;
        ThreadLocal::InitializeCleaner();
        ThreadLocal::InitializeCleaner();
        manager.UnbindMutator(*logical);
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        manager.BindMutator(*logical);
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    const bool observed = WaitNative([&] { return ready.load(std::memory_order_acquire); });
    size_t natives = 0, logicals = 0;
    bool classified = observed;
    if (observed) {
        ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator* owner, ThreadLocalData* tls) {
            if (&data == native) {
                ++natives;
                classified &= !data.managedOwner && owner == nullptr && tls != nullptr && tls->mutator == nullptr;
            }
            if (owner == logical) {
                ++logicals;
                classified &= data.managedOwner && tls == nullptr && &data != native;
            }
        });
    }
    std::fprintf(stderr, "NATIVE_LOGICAL_INVENTORY executed=1 repeated_attach=2 parked=1 natives=%zu logicals=%zu classified=%d\n",
                 natives, logicals, classified);
    release.store(true, std::memory_order_release);
    carrier.join();
    GC_EXPECT_TRUE(natives == 1 && logicals == 1 && classified);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, ConcurrentRemovers)
{
    InitNativeRuntime();
    std::atomic<ThreadGCData*> data[2]{};
    std::atomic<bool> finish{false}, joined[2]{};
    std::thread owners[2], joiners[2];
    for (size_t i = 0; i < 2; ++i) {
        owners[i] = std::thread([&, i] {
            ThreadLocal::InitializeCleaner();
            data[i].store(&ThreadLocal::GetGCData(), std::memory_order_release);
            while (!finish.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        });
    }
    const bool ready = WaitNative([&] { return data[0].load() && data[1].load(); });
    for (size_t i = 0; i < 2; ++i) {
        joiners[i] = std::thread([&, i] { owners[i].join(); joined[i].store(true, std::memory_order_release); });
    }
    bool removed = false, retained = false;
    {
        CleanThreadLocalData::Iterator oldReaders;
        const ThreadGCData* observed[2]{};
        uintptr_t masks[2]{};
        // nonJavaThread.cpp:69 reloads mutable next links. Acquire the actual
        // nodes before unlink; the read-side critical section protects these
        // nodes, not a snapshot of membership after concurrent list mutation.
        for (; !oldReaders.End(); oldReaders.Step()) {
            const auto* seen = &oldReaders.Current()->nativeData;
            for (size_t i = 0; i < 2; ++i) {
                if (seen == data[i].load()) { observed[i] = seen; masks[i] = seen->storeGoodMask; }
            }
        }
        finish.store(true, std::memory_order_release);
        removed = WaitNative([&] { return !ContainsNative(data[0].load()) && !ContainsNative(data[1].load()); });
        retained = !joined[0].load() && !joined[1].load() && observed[0] && observed[1];
        if (retained) {
            retained = observed[0]->storeGoodMask == masks[0] && observed[1]->storeGoodMask == masks[1];
        }
    }
    const bool completed = WaitNative([&] { return joined[0].load() && joined[1].load(); });
    std::fprintf(stderr, "NATIVE_CONCURRENT_REMOVERS executed=1 ready=%d removed=%d retained=%d completed=%d\n",
                 ready, removed, retained, completed);
    if (!completed) {
        // Preserve the target failure rather than hiding it behind join().
        std::fflush(stderr);
        _exit(1);
    }
    for (auto& thread : joiners) { thread.join(); }
    GC_EXPECT_TRUE(ready && removed && retained && completed);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
