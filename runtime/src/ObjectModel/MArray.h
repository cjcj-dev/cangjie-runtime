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
    // OpenJDK ZObjArrayAllocator uses the same 64 KiB maximum segment
    // (zObjArrayAllocator.cpp:55-63). The threshold includes the array header.
    static constexpr MSize LARGE_REF_ARRAY_INIT_SEGMENT_SIZE = 64 * 1024;

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

    inline MIndex GetMArraySize() const; // overall size in bytes of this array
    inline MSize GetElementSize() const; // in Bytes
    inline MIndex GetContentSize() const;
    inline bool IsPrimitiveArray() const;

    // access array elements
    template<typename T>
    inline T GetPrimitiveElement(MIndex index) const;

    template<typename T>
    inline void SetPrimitiveElement(MIndex index, T value);

    inline ObjectPtr GetRefElement(MIndex index);
    inline void SetRefElement(MIndex index, const ObjectPtr mObj);
    inline U8* ConvertToCArray() const;
    // this interface can only be called by array with reference fields.
    void ForEachRefFieldInRange(const RefFieldVisitor& visitor, MAddress fieldStart, MIndex fieldEnd) const;

private:
    static MArray* InitializeLargeRefArray(MAddress address, MSize arraySize, MIndex nElems,
                                           TypeInfo& arrayClass);

    // use MIndex because length is the upper boundary of all indices
    MIndex length;
    // array content is appended here.
};

#if defined(MRT_GC_UNIT_TESTS)
class Mutator;

enum class LargeArrayRootVisitSite : uint8_t {
    MUTATOR_STACK_NATIVE,
    MUTATOR_STACK_MANAGED,
    STACK_WATERMARK_NATIVE,
    STACK_WATERMARK_MANAGED,
    MINOR_MARK,
    MINOR_RELOCATE,
    REMEMBERED,
    ITERATOR_SKIP,
};

enum class LargeArrayRootPhase : uint8_t {
    MAJOR_MARK,
    MINOR_MARK,
};

// Test-only product hooks. They are compiled out of the default product shape;
// the deterministic suite still enters through MCC_NewObjArray in the product SO.
struct LargeArrayInitTestHooks {
    MAddress (*allocate)(size_t size, AllocType allocType) = nullptr;
    void (*onPublish)(MArray* array) = nullptr;
    void (*onYield)(size_t segmentIndex) = nullptr;
    void (*onWithdraw)(MArray* array) = nullptr;
    void (*onRootVisit)(LargeArrayRootVisitSite site, BaseObject* object) = nullptr;
    void (*onRootPhase)(LargeArrayRootPhase phase, Mutator* mutator, bool watermarkDone) = nullptr;
};

extern "C" MRT_EXPORT void CJ_MRT_SetLargeArrayInitTestHooks(const LargeArrayInitTestHooks* hooks);
extern "C" MRT_EXPORT MAddress CJ_MRT_TestAllocateArrayStorage(size_t size, AllocType allocType);
void NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite site, BaseObject* object);
void NoteLargeArrayInitRootPhase(LargeArrayRootPhase phase, Mutator* mutator, bool watermarkDone);
#endif
} // namespace MapleRuntime
#endif // MRT_MARRAY_H
