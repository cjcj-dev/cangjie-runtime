// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MARRAY_INLINE_H
#define MRT_MARRAY_INLINE_H

#include "ObjectModel/Field.inline.h"
#include "Inspector/CjAllocData.h"
// model interface
#include "ExceptionManager.h"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "HeapManager.inline.h"
// module internal interfaces
#include "MArray.h"
#include "Heap/z/zObjArrayAllocator.hpp"
#include "MClass.inline.h"

namespace MapleRuntime {
constexpr MOffset MArray::GetContentOffset() { return sizeof(MArray); }

inline MIndex MArray::GetLength() const { return length; }

inline void MArray::SetLength(MIndex number) { length = number; }

inline U8* MArray::ConvertToCArray() const
{
    // The receiver is an uncolored address supplied by the load barrier.
    return reinterpret_cast<uint8_t*>(reinterpret_cast<Uptr>(this) + MArray::GetContentOffset());
}

inline MIndex MArray::GetMArraySize() const { return (MArray::GetContentOffset() + GetContentSize()); }

inline MSize MArray::GetElementSize() const
{
    TypeInfo* componentTypeInfo = GetComponentTypeInfo();
    if (componentTypeInfo->IsRef()) {
        return sizeof(BaseObject*);
    }
    return componentTypeInfo->GetComponentSize();
}

inline MIndex MArray::GetContentSize() const
{
    auto len = GetLength();
    auto elementSize = GetElementSize();
    return elementSize * len;
}

inline bool MArray::IsPrimitiveArray() const
{
    TypeInfo* componentTypeInfo = GetTypeInfo()->GetComponentTypeInfo();
    return componentTypeInfo == nullptr ? false : componentTypeInfo->IsPrimitiveType();
}

template<typename Visitor>
void MArray::ForEachRefFieldInRange(const Visitor& visitor, MAddress fieldStart, MIndex fieldEnd) const
{
    // VM layout adapter. ZIterator's range entry requires visible ref arrays;
    // this byte-range form also supports Cangjie's inline struct-array copies.
    TypeInfo* componentTi = GetComponentTypeInfo();
    MIndex size = fieldEnd - fieldStart;
    if (componentTi->IsStructType()) {
        GCTib gcTib = componentTi->GetGCTib();
        size_t elementSize = GetElementSize();
        CHECK(elementSize != 0);
        MIndex limit = size / elementSize;
        for (MIndex i = 0; i < limit; ++i) {
            gcTib.ForEachBitmapWord(fieldStart, visitor);
            fieldStart += elementSize;
        }
    } else if (componentTi->IsObjectType() || componentTi->IsArrayType() || componentTi->IsInterface()) {
        HeapSlot<false>* arrayContent = &HeapSlotAt<false>(fieldStart);
        MIndex upLimit = size / sizeof(RefField<>);
        for (MIndex i = 0; i < upLimit; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", this);
    }
}

inline ObjectPtr MArray::GetRefElement(MIndex index)
{
    RefField<>& ref = GetRefField(MArray::GetContentOffset() + RefField<>::GetSize() * index);
    return ZBarrier::ReadReference(this, ref);
}

inline void MArray::SetRefElement(MIndex index, const ObjectPtr mObj)
{
    RefField<>& ref = GetRefField(MArray::GetContentOffset() + RefField<>::GetSize() * index);
    ZBarrier::WriteReference(this, ref, mObj);
}

template<typename T>
inline T MArray::GetPrimitiveElement(MIndex index) const
{
    Field<T>& field = GetField<T>(MArray::GetContentOffset() + GetElementSize() * index);
    // normally we do not barrier for reading primitive fields.
    return field.GetFieldValue();
}

template<typename T>
inline void MArray::SetPrimitiveElement(MIndex index, T value)
{
    Field<T>& field = GetField<T>(MArray::GetContentOffset() + GetElementSize() * index);
    field.SetFieldValue(this, value);
}

static inline MIndex CalculateArraySize(MIndex nElems, const U32 elemBytes)
{
    if (elemBytes == 0) {
        return static_cast<U64>(MArray::GetContentOffset());
    }
    U64 maxLength = (MAX_ARRAY_SIZE - MArray::GetContentOffset()) / elemBytes;
    U64 totalSize = static_cast<U64>(nElems) * elemBytes + static_cast<U64>(MArray::GetContentOffset());
    if (UNLIKELY(nElems >= maxLength)) {
        // check overflow
        return MAX_ARRAY_SIZE;
    }
    return static_cast<MIndex>(totalSize);
}

inline MArray* MArray::NewArray(MIndex nElems, TypeInfo& arrayClass, AllocType allocType)
{
    DCHECK_D(arrayClass.IsArrayType(), "Expect an array type");
    MArray* newArray = nullptr;
    if (arrayClass.GetComponentTypeInfo()->IsObjectType()) {
        newArray = NewRefArray(nElems, arrayClass, allocType);
    } else {
        // component is value type
        auto elem = arrayClass.GetSuperTypeInfo();
        auto elemBits = sizeof(void*);
        if (!elem->IsRef()) {
            elemBits = arrayClass.GetSuperTypeInfo()->GetComponentSize();
        }
        newArray = NewKnownWidthArray(nElems, arrayClass, elemBits, allocType);
    }
    return newArray;
}

inline MArray* MArray::NewRefArray(MIndex nElems, TypeInfo& arrayClass, AllocType allocType)
{
    return NewKnownWidthArray(nElems, arrayClass, RefField<>::GetSize(), allocType);
}

inline MArray* MArray::NewKnownWidthArray(MIndex nElems, TypeInfo& arrayClass, const U32 elemBytes, AllocType allocType)
{
    DCHECK_D(arrayClass.IsArrayType(), "Expect an array type");
    MIndex arraySize = CalculateArraySize(nElems, elemBytes);
    if (UNLIKELY(arraySize == MAX_ARRAY_SIZE || arraySize > Heap::GetHeap().GetMaxCapacity())) {
        ExceptionManager::OutOfMemory();
        return nullptr;
    }
    const bool useSegmentedClear = arraySize > LARGE_ARRAY_INIT_SEGMENT_SIZE &&
        allocType == AllocType::MOVEABLE_OBJECT &&
        (arrayClass.GetComponentTypeInfo()->IsPrimitiveType() ||
         (elemBytes == RefField<>::GetSize() && arrayClass.GetComponentTypeInfo()->IsRef()));
    MAddress address = HeapManager::Allocate(
        arraySize, useSegmentedClear ? AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR : allocType);
    if (LIKELY(address != NULL_ADDRESS)) {
        if (UNLIKELY(useSegmentedClear)) {
            return ZObjArrayAllocator(address, arraySize, nElems, arrayClass).initialize();
        }
        MArray* newArray = reinterpret_cast<MArray*>(SetClassInfo(address, &arrayClass));
        newArray->SetLength(nElems);
#if defined(__OHOS__) && (__OHOS__ == 1)
        if (CjAllocData::GetCjAllocData()->IsRecording()) {
            CjAllocData::GetCjAllocData()->RecordAllocNodes(&arrayClass, arraySize);
        }
#endif
        return newArray;
    }
    return nullptr;
}
} // namespace MapleRuntime
#endif // MRT_MARRAY_INLINE_H
