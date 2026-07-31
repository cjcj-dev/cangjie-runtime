// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MARRAY_H
#define MRT_MARRAY_H

#include "Common/BaseObject.h"
namespace MapleRuntime {
#ifdef POINTER_IS_64BIT
// align to 8 bytes for 64-bit platforms
class ATTR_PACKED(8) MArray : public BaseObject {
#else
class ATTR_PACKED(4) MArray : public BaseObject {
#endif
public:
    static constexpr MOffset GetContentOffset(); // in Bytes

    // inlined static functions to create arrays
    inline static MArray* NewArray(MIndex nElems, TypeInfo& arrayClass,
                                   AllocType allocType = AllocType::MOVEABLE_OBJECT);
    inline static MArray* NewRefArray(MIndex nElems, TypeInfo& arrayClass,
                                      AllocType allocType = AllocType::MOVEABLE_OBJECT);
    inline static MArray* NewKnownWidthArray(MIndex nElems, TypeInfo& arrayClass, const U32 elemBytes,
                                             AllocType allocType = AllocType::MOVEABLE_OBJECT);

    // inlined functions
    inline MIndex GetLength() const; // the number of elements
    inline void SetLength(MIndex number);

    inline ObjectPtr GetRefElement(MIndex index);
    inline void SetRefElement(MIndex index, const ObjectPtr mObj);
    inline U8* ConvertToCArray() const;
private:
    // This implementation consumes component type information and is exposed
    // only through the CurrentPtr overload in MArray.inline.h.
    void ForEachRefFieldInRange(const RefFieldVisitor& visitor, MAddress fieldStart, MIndex fieldEnd) const;
    friend void ForEachRefFieldInRange(CurrentPtr object, const RefFieldVisitor& visitor, MAddress fieldStart,
                                       MIndex fieldEnd);

    // use MIndex because length is the upper boundary of all indices
    MIndex length;
    // array content is appended here.
};
} // namespace MapleRuntime
#endif // MRT_MARRAY_H
