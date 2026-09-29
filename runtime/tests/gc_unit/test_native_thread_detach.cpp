// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/ThreadLocal.h"
#include "TypeInfoManager.h"
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
    p.gcParam.youngGCThreads = 1;
    p.gcParam.oldGCThreads = 1;
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

GC_RUNTIME_OTHER_VM_TEST(NativeOwner1286, FinalPublication)
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
    native1286_allow_exit.store(debugger ? 0 : 1);
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
    if (!debugger) { producer.join(); }
    const bool afterMark = ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED");
    if (debugger) { producer.join(); }
    page = Heap::page(reinterpret_cast<uintptr_t>(native1286_object));
    const bool marked = page != nullptr && page->is_object_marked(from_object(native1286_object), false);
    const bool removed = !ContainsNative(native1286_data);
    std::fprintf(stderr, "NATIVE_FINAL_PUBLICATION executed=1 reached=%d old=%d before=%d ready=%d private=%zu marked=%d removed=%d after_mark=%d\n",
                 reached, old, before, ready, privateEntries, marked, removed, afterMark);
#if defined(MRT_TESTABLE_INTERNALS)
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(nullptr);
    const bool push = nativePushObserved.load();
    std::fprintf(stderr, "NATIVE_PUSH_WITNESS observed=%d\n", push);
#endif
    // Evaluate the product outcome first, before setup assertions.
    GC_EXPECT_TRUE(marked && removed);
    GC_EXPECT_TRUE(reached && old && !before && ready && privateEntries == 1 && afterMark);
#if defined(MRT_TESTABLE_INTERNALS)
    GC_EXPECT_TRUE(push);
#endif
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

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
    bool found = false, removed = false, waiting = false, retained = false;
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
    std::fprintf(stderr, "NATIVE_ITERATOR_GRACE executed=1 found=%d unlinked=%d waiting=%d retained=%d later_reader=%d completed=%d\n",
                 found, removed, waiting, retained, lateReady.load(), progressed);
    lateRelease.store(true, std::memory_order_release);
    late.join();
    joiner.join();
    GC_EXPECT_TRUE(found && removed && waiting && retained && progressed);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
