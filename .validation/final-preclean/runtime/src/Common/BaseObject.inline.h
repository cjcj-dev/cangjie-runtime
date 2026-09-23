// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_BASE_OBJECT_INLINE_H
#define MRT_BASE_OBJECT_INLINE_H

#include "Common/BaseObject.h"
#include "ObjectModel/MArray.inline.h"

namespace MapleRuntime {
// ZGC zIterator.inline.hpp:59-84 and oops/oop.inline.hpp:374-377:
// preserve the concrete closure through VM layout dispatch. Cangjie records
// and inline record arrays use GCTib instead of HotSpot Klass oop maps.
template<typename Visitor>
static void ForEachRefFieldInNonArrayObject(ObjectPtr obj, TypeInfo* klass, const Visitor& visitor)
{
    GCTib gcTib = klass->GetGCTib();
    // gcTib record payload data, skip the TypeInfo
    MAddress objAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    gcTib.ForEachBitmapWord(objAddr, visitor);
}

// Call func on each element in an object array.
template<typename Visitor>
static void ForEachElementInArray(ObjectPtr obj, TypeInfo* klass, const Visitor& visitor)
{
    // take array length and content.
    MArray* mArray = reinterpret_cast<MArray*>(obj);
    MIndex arrayLengthVal = mArray->GetLength();
    TypeInfo* componentTypeInfo = klass->GetComponentTypeInfo();
    if (componentTypeInfo->IsStructType()) {
        GCTib gcTib = componentTypeInfo->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(mArray) + MArray::GetContentOffset();
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            gcTib.ForEachBitmapWord(contentAddr, visitor);
            contentAddr += componentTypeInfo->GetComponentSize();
        }
    } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
               componentTypeInfo->IsInterface()) {
        HeapSlot<>* arrayContent = &HeapSlotAt<>(mArray->ConvertToCArray());
        // for each object in array.
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", mArray);
    }
}

template<typename Visitor>
void BaseObject::ForEachRefField(const Visitor& visitor)
{
    ForEachRefField(visitor, GetTypeInfo());
}

template<typename Visitor>
void BaseObject::ForEachRefField(const Visitor& visitor, TypeInfo* typeInfo)
{
    // VM layout dispatch only. GC's safe/unsafe split is in ZIterator,
    // as in zIterator.inline.hpp:64-77 and oopDesc::oop_iterate.
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachElementInArray(this, typeInfo, visitor);
        } else {
            ForEachRefFieldInNonArrayObject(this, typeInfo, visitor);
        }
    }
};

template<typename Visitor>
void BaseObject::ForEachRefInStruct(const Visitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    TypeInfo* typeInfo = GetTypeInfo();
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachAggRefFieldInArray(visitor, aggStart, aggEnd);
        } else {
            ForEachAggRefFieldInNonArray(visitor, aggStart, aggEnd);
        }
    }
}

template<typename Visitor>
void BaseObject::ForEachAggRefFieldInArray(const Visitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    // take array length and content.
    MArray* mArray = static_cast<MArray*>(this);
    MIndex arrayLen = mArray->GetLength();
    TypeInfo* component = mArray->GetComponentTypeInfo();
    if (component->IsStructType()) {
        GCTib gcTib = component->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(this) + MArray::GetContentOffset();
        size_t contentSize = mArray->GetElementSize();
        // MIndex is enough to describe the size;
        MIndex startIndex = static_cast<MIndex>((aggStart - contentAddr) / contentSize);
        size_t alignedStart = startIndex * contentSize + contentAddr;
        MRT_ASSERT((alignedStart + contentSize) >= aggEnd, "aggregate element is not align\n");
        MAddress currentAddr = alignedStart;
        for (U64 i = startIndex; (i < arrayLen) && (currentAddr < aggEnd); ++i) {
            gcTib.ForEachBitmapWordInRange(currentAddr, visitor, aggStart, aggEnd);
            currentAddr += contentSize;
        }
    } else {
        LOG(RTLOG_FATAL, "this interface mustn't be invoked by array whose element is not record");
    }
}

template<typename Visitor>
void BaseObject::ForEachAggRefFieldInNonArray(const Visitor& visitor, MAddress aggStart, MAddress aggEnd) const
{
    // gcTib record payload data, skip the TypeInfo
    GetGCTib().ForEachBitmapWordInRange(reinterpret_cast<MAddress>(this) + TYPEINFO_PTR_SIZE, visitor, aggStart,
                                        aggEnd);
}

} // namespace MapleRuntime
#endif // MRT_BASE_OBJECT_INLINE_H
