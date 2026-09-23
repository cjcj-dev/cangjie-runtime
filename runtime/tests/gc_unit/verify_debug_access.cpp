// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone Debug product-entry fixture; it is intentionally not a second
// implementation of the barrier. The reader below is imported from the SO.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zVerify.hpp"
#include "ObjectModel/MObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"
#include <cstdio>
#include <cstring>
using namespace MapleRuntime;
extern "C" BaseObject* CJ_MCC_LoadBarrierOnOopFieldPreloaded(BaseObject*, volatile zpointer*);
int main(int argc, char** argv)
{
    if (argc != 2 || !ZVerifyOops) { return 78; }
    RuntimeParam param{};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    if (InitCJRuntime(&param) != E_OK) { return 79; }
    auto& manager = MutatorManager::Instance();
    Mutator* mutator = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    if (mutator == nullptr) { return 80; }
    const bool invalid = std::strcmp(argv[1], "invalid") == 0;
    const bool valid = std::strcmp(argv[1], "valid") == 0;
    const bool safe = std::strcmp(argv[1], "saferegion") == 0;
    NativeSlot slot(zpointer::null);
    BaseObject* expected = nullptr;
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    if (invalid || valid) {
        ScopedObjectAccess access;
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(sizeof(uintptr_t));
        expected = MObject::NewPinnedObject(type, 2 * sizeof(uintptr_t));
        if (expected == nullptr) { return 81; }
        ZPage* page = Heap::page(reinterpret_cast<MAddress>(expected));
        const MAddress address = invalid ? page->GetRegionAllocPtr() : reinterpret_cast<MAddress>(expected);
        if (invalid && (Heap::is_in(address) || address >= page->GetRegionEnd())) { return 83; }
        // The invalid address is committed/readable but outside the allocated
        // interval, so ZVerifyOops, not the earlier dereferenceability check, decides.
        slot.StoreColoured(ZAddress::store_good(static_cast<zaddress>(address)));
    }
    BaseObject* result;
    if (safe) {
        (void)mutator->EnterSaferegion(false);
        result = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
            reinterpret_cast<BaseObject*>(raw(slot.GetFieldValue())), reinterpret_cast<volatile zpointer*>(&slot));
    } else {
        ScopedObjectAccess access;
        result = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
            reinterpret_cast<BaseObject*>(raw(slot.GetFieldValue())), reinterpret_cast<volatile zpointer*>(&slot));
    }
    std::fprintf(stderr, "DEBUG_PRODUCT_ACCESS_RETURNED mode=%s result=%p\n", argv[1], result);
    if (!invalid && result != expected) { return 82; }
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    return 0;
}
