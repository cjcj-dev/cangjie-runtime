// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// ZGC zCollectedHeap.cpp:153-155 and zRelocationSetSelector.inline.hpp:75-114.
// Exercise the compiler allocation entry and real collection requests against
// a separately built product SO. No synthetic page/mark/selection state.
#include "Cangjie.h"
#include "CompilerCalls.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "TypeInfoManager.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
using namespace MapleRuntime;

int main(int argc, char** argv)
{
    if (argc != 2) { return 80; }
    const bool empty = std::strcmp(argv[1], "empty") == 0;
    const bool census = std::strcmp(argv[1], "census-control") == 0;
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    if (InitCJRuntime(&params) != E_OK) { return 81; }
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    if (!manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD)) { return 82; }
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    constexpr size_t size = 1024;
    type->SetInstanceSize(size - sizeof(uintptr_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    std::vector<U64> roots;
    std::vector<uintptr_t> original;
    std::set<uintptr_t> pages;
    size_t allocated = 0;
    {
        ScopedObjectAccess access;
        for (size_t i = 0; i < 24 * 1024; ++i) {
            BaseObject* obj = MCC_NewPinnedObject(type, size, false);
            if (obj == nullptr) { return 83; }
            ++allocated;
            auto* page = Heap::page(reinterpret_cast<uintptr_t>(obj));
            if (pages.insert(page->GetRegionStart()).second && !empty) {
                roots.push_back(heap.RegisterExportRoot(obj));
                original.push_back(reinterpret_cast<uintptr_t>(obj));
            }
            if (census) {
                // Intentional impossible ordinary allocation causes the
                // temporary diagnostic SO to report a positive pinned input.
                type->SetInstanceSize(128 * 1024 * 1024 - sizeof(uintptr_t));
                (void)MCC_NewObject(type, 128 * 1024 * 1024);
                return 84;
            }
        }
    }
    const size_t before = heap.page_allocator().GetUsedBytes();
    heap.RequestGC(GC_REASON_YOUNG, false);
    heap.RequestGC(GC_REASON_USER, false);
    const size_t after = heap.page_allocator().GetUsedBytes();
    size_t moved = 0, valid = 0, reclaimed = 0;
    {
        ScopedObjectAccess access;
        for (size_t i = 0; i < roots.size(); ++i) {
            BaseObject* obj = heap.GetExportObject(roots[i]);
            valid += obj != nullptr && obj->GetTypeInfo() == type;
            moved += reinterpret_cast<uintptr_t>(obj) != original[i];
            heap.RemoveExportObject(roots[i]);
        }
        for (uintptr_t page : pages) { reclaimed += Heap::page(page) == nullptr; }
    }
    // All target invariants are evaluated before choosing the exit status.
    const bool reclaimedBytes = after < before;
    const bool retainedRoots = valid == roots.size();
    const bool relocated = empty || moved > 0;
    std::fprintf(stderr, "PINNED_RECLAIM_TARGET mode=%s allocated=%zu pages=%zu roots=%zu valid=%zu "
                 "moved=%zu removed_pages=%zu before=%zu after=%zu reclaimed=%d retained=%d relocated=%d\n",
                 argv[1], allocated, pages.size(), roots.size(), valid, moved, reclaimed,
                 before, after, reclaimedBytes, retainedRoots, relocated);
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    const int result = reclaimedBytes && retainedRoots && relocated ? 0 : 1;
    if (FiniCJRuntime() != E_OK) { return 85; }
    return result;
}
