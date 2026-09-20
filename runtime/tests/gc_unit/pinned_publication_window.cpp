// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// External GDB-driven fixture: stop the PRODUCT between page acquisition and
// publication, run a real collection, then resume the unmodified allocator.
// This executable is deliberately not an ordinary runner case: without the
// debugger's GC intervention its ordering assertion must fail, never skip.
#include <cstdio>
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
using namespace MapleRuntime;

extern "C" {
volatile unsigned long long pinned_window_before = 0;
volatile unsigned long long pinned_window_after = 0;
volatile uintptr_t pinned_window_page = 0;
volatile unsigned pinned_window_injected = 0;
}

// Debugger-only driver. It does not allocate, publish, or alter page state.
extern "C" __attribute__((noinline)) void PinnedWindowCollect(void* page)
{
    auto& heap = Heap::GetHeap();
    pinned_window_page = reinterpret_cast<uintptr_t>(page);
    pinned_window_before = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    heap.RequestGC(GC_REASON_USER, false);
    pinned_window_after = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    ++pinned_window_injected;
    std::fprintf(stderr, "GDB_REAL_GC_WINDOW page=%p before=%llu after=%llu\n", page,
                 pinned_window_before, pinned_window_after);
}

int main()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    if (InitCJRuntime(&params) != E_OK) { return 100; }
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(void*));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    bool ordering, current, shared;
    U64 root;
    MObject* first;
    {
        ScopedObjectAccess access;
        first = MObject::NewPinnedObject(type, 2 * sizeof(void*));
        ZPage* page = first == nullptr ? nullptr : Heap::page(reinterpret_cast<uintptr_t>(first));
        ordering = pinned_window_injected == 1 && pinned_window_before < pinned_window_after &&
                   reinterpret_cast<uintptr_t>(page) == pinned_window_page;
        current = page != nullptr && page->IsAllocating();
        MObject* second = MObject::NewPinnedObject(type, 2 * sizeof(void*));
        shared = first != nullptr && reinterpret_cast<uintptr_t>(second) == reinterpret_cast<uintptr_t>(first) + 16;
        root = heap.RegisterExportRoot(first);
        std::fprintf(stderr, "PINNED_PUBLICATION_TARGET ordering=%d current=%d shared=%d page=%p "
                     "before=%llu after=%llu birth=%llu owner=%llu injected=%u\n", ordering, current, shared, page,
                     pinned_window_before, pinned_window_after,
                     page == nullptr ? 0 : (unsigned long long)page->BirthSequence(),
                     page == nullptr ? 0 : (unsigned long long)page->GetSnapshotEpoch(), pinned_window_injected);
    }
    heap.RequestGC(GC_REASON_USER, false);
    const bool retained = heap.GetExportObject(root) == first;
    std::fprintf(stderr, "PINNED_RETENTION_TARGET retained=%d\n", retained);
    heap.RemoveExportObject(root);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    const int result = ordering && current && shared && retained ? 0 : 1;
    if (FiniCJRuntime() != E_OK) { return 103; }
    return result;
}
