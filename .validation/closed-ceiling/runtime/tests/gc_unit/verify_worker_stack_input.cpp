// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// GDB input fixture: all GC execution remains in the linked product SO.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "ObjectModel/MObject.h"
#include "ObjectModel/RefField.inline.h"
#include <cstdio>
using namespace MapleRuntime;

// Test ELF symbols name an already allocated input and its real owner. GDB
// writes only the owner's existing stack slot at the coordinator entry.
ZMark* p16_mark = nullptr;
ThreadGCData* p16_worker = nullptr;
MarkStripeStack* p16_prepared = nullptr;

int main()
{
    RuntimeParam param{};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    if (InitCJRuntime(&param) != E_OK) { return 121; }
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uintptr_t));
    auto* object = MObject::NewPinnedObject(type, 2 * sizeof(uintptr_t));
    if (object == nullptr) { return 122; }
    auto& heap = Heap::GetHeap();
    auto& roots = heap.GetFinalizerProcessor().StrongRootStorage();
    NativeSlot* root = roots.Allocate();
    if (root == nullptr) { return 123; }
    root->StoreColoured(ZAddress::store_good(from_object(object)));
    ConcurrentGCBreakpoints::AcquireControl();
    if (!ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED")) { return 124; }
    p16_mark = &heap.old().Mark();
    heap.old().Workers()->threads_do([](WorkerThread* worker) {
        if (p16_worker == nullptr) { p16_worker = worker->gc_data(); }
    });
    if (p16_worker == nullptr || !p16_worker->markStacks[1].IsEmpty()) { return 125; }
    p16_prepared = MarkStripeStack::Create(true);
    if (p16_prepared == nullptr) { return 126; }
    p16_prepared->Push(MarkStackEntry(raw(ZAddress::offset(from_object(object))), false, false, false, false));
    std::fprintf(stderr, "VERIFY_WORKER_INPUT_READY mark=%p owner=%p stack=%p object=%p\n",
                 p16_mark, p16_worker, p16_prepared, object);
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    BaseObject* actual = ZBarrier::ReadStaticRef(*root);
    const bool matched = actual == object && p16_worker->markStacks[1].IsEmpty();
    std::fprintf(stderr, "VERIFY_WORKER_COMPLETION_ASSERT_EXECUTED root=%p expected=%p matched=%d\n",
                 actual, object, matched);
    roots.Release(root);
    return matched ? 0 : 127;
}
