// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_Z_ITERATOR_INLINE_HPP
#define MRT_Z_ITERATOR_INLINE_HPP

#include "Heap/z/zIterator.hpp"
#include "Heap/z/zVerify.hpp"
#include "ObjectModel/MArray.inline.h"

namespace MapleRuntime {

inline bool ZIterator::is_invisible_object(BaseObject* object)
{
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    z_verify_safepoints_are_blocked();
#endif
    return object->IsInvisibleObject();
}

inline bool ZIterator::is_invisible_object_array(BaseObject* object)
{
    return is_invisible_object_array(object, object->GetTypeInfo());
}

inline bool ZIterator::is_invisible_object_array(BaseObject* object, TypeInfo* klass)
{
    // TypeInfo adapter for Klass::is_refArray_klass(). Inline value arrays
    // and primitive arrays are not reference arrays.
    const bool referenceArray = klass->IsRawArray() && klass->GetComponentTypeInfo()->IsRef();
    return referenceArray && is_invisible_object(object);
}

template <typename OopClosureT>
void OopIteratorClosureDispatch::oop_oop_iterate(OopClosureT* closure, BaseObject* object, TypeInfo* klass)
{
    // Cangjie's VM field-layout dispatch uses TypeInfo/GCTib. In particular,
    // honor the caller-supplied klass rather than reloading the object header.
    object->ForEachRefField([&](RefField<>& field) { closure->do_oop(&field); }, klass);
}

template <typename OopClosureT>
void ZIterator::oop_iterate_safe(BaseObject* object, OopClosureT* closure)
{
    oop_iterate_safe(object, object->GetTypeInfo(), closure);
}

template <typename OopClosureT>
void ZIterator::oop_iterate_safe(BaseObject* object, TypeInfo* klass, OopClosureT* closure)
{
    // ZGC zIterator.inline.hpp:64-70: the sole invisible-array split is
    // here, before VM closure dispatch, not in mark or in a range iterator.
    if (!is_invisible_object_array(object, klass)) {
        OopIteratorClosureDispatch::oop_oop_iterate(closure, object, klass);
    }
}
}

template <typename OopClosureT>
void ZIterator::oop_iterate(BaseObject* object, OopClosureT* closure)
{
    // zIterator.inline.hpp:74-77: this entry requires a visible object.
    DCHECK(!is_invisible_object_array(object));
    object->ForEachRefField([&](RefField<>& field) { closure->do_oop(&field); });
}

template <typename OopClosureT>
void ZIterator::oop_iterate_elements_range(MArray* object, OopClosureT* closure, MIndex start, MIndex end)
{
    // zIterator.inline.hpp:80-84: indices are elements, and range is not safe.
    DCHECK(!is_invisible_object_array(object));
    DCHECK(object->GetTypeInfo()->IsRawArray() && object->GetComponentTypeInfo()->IsRef());
    const MAddress first = reinterpret_cast<MAddress>(object->ConvertToCArray());
    object->ForEachRefFieldInRange([&](RefField<>& field) { closure->do_oop(&field); },
                                  first + start * sizeof(RefField<>), first + end * sizeof(RefField<>));
}

template <typename Function>
void ZIterator::basic_oop_iterate_safe(BaseObject* object, Function function)
{
    basic_oop_iterate_safe(object, object->GetTypeInfo(), function);
}

template <typename Function>
void ZIterator::basic_oop_iterate_safe(BaseObject* object, TypeInfo* klass, Function function)
{
    ZBasicOopIterateClosure<Function> closure(function);
    oop_iterate_safe(object, klass, &closure);
}

template <typename Function>
void ZIterator::basic_oop_iterate(BaseObject* object, Function function)
{
    ZBasicOopIterateClosure<Function> closure(function);
    oop_iterate(object, &closure);
}

} // namespace MapleRuntime
#endif
