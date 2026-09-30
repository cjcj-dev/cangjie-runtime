// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_M_REFERENCE_H
#define MRT_M_REFERENCE_H

#include "Common/BaseObject.inline.h"
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zReferenceDiscoverer.hpp"

namespace MapleRuntime {
// java_lang_ref_Reference's VM accessors. TypeInfo offsets describe the
// compiler-declared payload fields; no storage is added by the allocator.
class MReference {
    enum Field : uint16_t { REFERENT, QUEUE, NEXT, DISCOVERED };
    static HeapSlot<>* field_addr(BaseObject* object, TypeInfo* klass, Field field)
    {
        return &HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE + klass->GetFieldOffset(field));
    }
public:
    static HeapSlot<>* referent_addr(BaseObject* object, TypeInfo* klass)
    {
        return field_addr(object, klass, REFERENT);
    }
    static HeapSlot<>* referent_addr(BaseObject* object) { return referent_addr(object, object->GetTypeInfo()); }
    static HeapSlot<>* discovered_addr(BaseObject* object, TypeInfo* klass)
    {
        return field_addr(object, klass, DISCOVERED);
    }
    static HeapSlot<>* discovered_addr(BaseObject* object) { return discovered_addr(object, object->GetTypeInfo()); }
    static HeapSlot<>* next_addr(BaseObject* object) { return field_addr(object, object->GetTypeInfo(), NEXT); }
    static BaseObject* discovered(BaseObject* object) { return HeapAccess<>::oop_load(discovered_addr(object)); }
    static void set_discovered(BaseObject* object, BaseObject* value)
    {
        HeapAccess<>::oop_store(discovered_addr(object), value);
    }
    static BaseObject* next(BaseObject* object) { return HeapAccess<>::oop_load(next_addr(object)); }
    static void set_next(BaseObject* object, BaseObject* value) { HeapAccess<>::oop_store(next_addr(object), value); }
    static ReferenceType reference_type(TypeInfo* klass)
    {
        return klass->IsFinalReferenceType() ? ReferenceType::FINAL : ReferenceType::WEAK;
    }
};
}
#endif
