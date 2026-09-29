// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone debugger fixture: the actual young collector has no ZBreakpoint
// phase callback. check_native_detach.py --young stops its existing methods.
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include <atomic>
#include <cstdio>
#include <thread>
using namespace MapleRuntime;
namespace MapleRuntime { extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize); }
extern "C" {
std::atomic<int> native1286_allow_exit{0}, native1286_start_producer{0}, native1286_check_now{0};
ThreadGCData* native1286_data = nullptr;
BaseObject* native1286_object = nullptr;
size_t native1286_private = 0;
bool native1286_before = false, native1286_marked = false, native1286_removed = false;
__attribute__((noinline)) void native1286_ready() { std::fprintf(stderr, "NATIVE1286_YOUNG_READY\n"); }
__attribute__((noinline)) void native1286_produced() { std::fprintf(stderr, "NATIVE1286_YOUNG_PRODUCED\n"); }
__attribute__((noinline)) void native1286_checked() { std::fprintf(stderr, "NATIVE1286_YOUNG_CHECKED\n"); }
}
namespace {
void* Allocate(void*)
{
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(32 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    native1286_object = MCC_NewObject(type, 32);
    mutator->SetManagedContext(true);
    return nullptr;
}
void ObservePush()
{
    if (&ThreadLocal::GetGCData() == native1286_data) {
        std::fprintf(stderr, "NATIVE_YOUNG_PUSH_CREATED data=%p native=%p\n",
                     static_cast<void*>(native1286_data), static_cast<void*>(ThreadLocal::GetThreadLocalData()->nativeGCData));
    }
}
}
int main()
{
    // Unlike gc_unit_main, this executable has not called ZGlobalsPointers
    // before entering the fixture. Preserve the zero -> attached assertion.
    std::atomic<bool> bootstrapReady{false}, retryBootstrap{false};
    bool absent = false, attached = false, stable = false;
    std::thread bootstrap([&] {
        ThreadLocal::InitializeCleaner();
        auto* data = ThreadLocal::GetThreadLocalData()->nativeGCData;
        bool listed = false;
        for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) { listed |= &it.Current()->nativeData == data; }
        absent = data->storeGoodMask == 0 && !listed;
        bootstrapReady.store(true, std::memory_order_release);
        while (!retryBootstrap.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        ThreadLocal::InitializeCleaner();
        for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) { attached |= &it.Current()->nativeData == data; }
        attached &= data->storeGoodMask != 0;
        const auto masks = data->storeGoodMask;
        ThreadLocal::InitializeCleaner();
        stable = data == &ThreadLocal::GetGCData() && data->storeGoodMask == masks;
    });
    while (!bootstrapReady.load(std::memory_order_acquire)) { std::this_thread::yield(); }
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
    if (InitCJRuntime(&p) != E_OK) { return 2; }
    retryBootstrap.store(true, std::memory_order_release);
    bootstrap.join();
    std::fprintf(stderr, "NATIVE_BOOTSTRAP_RETRY executed=1 absent=%d attached=%d stable=%d\n", absent, attached, stable);
    if (!(absent && attached && stable)) { return 4; }
    ConcurrentGCBreakpoints::AcquireControl();
    auto task = RunCJTask(Allocate, nullptr);
    void* ret = nullptr;
    if (task == nullptr || GetTaskRet(task, &ret) != E_OK) { return 3; }
    ReleaseHandle(task);
    std::atomic<bool> ready{false};
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(ObservePush);
    std::thread producer([&] {
        ThreadLocal::InitializeCleaner();
        native1286_data = &ThreadLocal::GetGCData();
        ready.store(true, std::memory_order_release);
        while (!native1286_start_producer.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        native1286_before = Heap::page(reinterpret_cast<uintptr_t>(native1286_object))->is_object_marked(from_object(native1286_object), false);
        ZBarrier::native_store_slow_path(from_object(native1286_object));
        native1286_private = native1286_data->markStacks[0].Population();
        native1286_produced();
        while (!native1286_allow_exit.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    });
    while (!ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    native1286_ready();
    std::thread collector([] { Heap::GetHeap().RequestGC(GC_REASON_YOUNG); });
    while (!native1286_check_now.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    producer.join();
    auto* page = Heap::page(reinterpret_cast<uintptr_t>(native1286_object));
    native1286_marked = page != nullptr && page->is_object_marked(from_object(native1286_object), false);
    native1286_removed = true;
    for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) {
        native1286_removed &= &it.Current()->nativeData != native1286_data;
    }
    std::fprintf(stderr, "NATIVE_YOUNG_FINAL_PUBLICATION executed=1 before=%d private=%zu marked=%d removed=%d\n",
                 native1286_before, native1286_private, native1286_marked, native1286_removed);
    native1286_checked();
    collector.join();
    MarkThreadLocalStacks::SetPushCreatedBreakpoint(nullptr);
    ConcurrentGCBreakpoints::ReleaseControl();
    const int fini = FiniCJRuntime();
    return !native1286_before && native1286_private == 1 && native1286_marked && native1286_removed && fini == E_OK ? 0 : 1;
}
