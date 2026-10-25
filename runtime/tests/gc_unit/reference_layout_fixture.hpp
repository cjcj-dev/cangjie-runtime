// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "gc_heap_fixture.hpp"
#include "ObjectModel/MReference.h"
namespace MapleRuntime { namespace GcUnit {
// Compiler ABI input for component/phase tests; this does not replace a managed
// registration or a managed finalizer. The end-to-end test compiles std.core.
struct ReferenceLayoutFixture {
    alignas(TypeInfo) unsigned char metadata[sizeof(TypeInfo)]{};
    U32 offsets[4] {0, 8, 16, 24};
    TypeInfo* type = reinterpret_cast<TypeInfo*>(metadata);
    explicit ReferenceLayoutFixture(bool final = false)
    {
        type->SetType(final ? TypeKind::TYPE_KIND_FINALREF_CLASS : TypeKind::TYPE_KIND_WEAKREF_CLASS);
        type->SetFieldNum(4);
        type->SetOffsets(offsets);
        type->SetInstanceSize(4 * sizeof(void*));
        type->SetFlagHasRefField();
        GCTib tib{};
        tib.tag = SIGN_BIT | 15;
        type->SetGCTib(tib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(metadata), sizeof(metadata));
    }
    BaseObject* Place(MAddress address, BaseObject* target)
    {
        auto* object = reinterpret_cast<BaseObject*>(address);
        object->SetClassInfo(type);
        for (U32 offset : offsets) {
            HeapSlotAt<>(address + TYPEINFO_PTR_SIZE + offset).StoreColoured(StoreGoodPointer(nullptr));
        }
        MReference::referent_addr(object)->StoreColoured(StoreGoodPointer(target));
        return object;
    }
};
} }
