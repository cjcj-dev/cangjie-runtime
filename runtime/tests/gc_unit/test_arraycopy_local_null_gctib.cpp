// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// CJ_MCC_ArrayCopyStruct's local path copies each element through
// CopyMaybeLocalStruct with the component GCTib. A reference-free component has
// GCTib tag 0 and a null StdGCTib*, so the element must be copied as plain bytes
// without walking a bitmap, as ZGC's TypeArrayKlass::copy_array does
// (hotspot/share/oops/typeArrayKlass.cpp:177).
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.inline.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Mutator/Mutator.h"
#include <cstdint>
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
extern "C" void CJ_MCC_ArrayCopyStruct(ObjectPtr, MAddress, size_t, ObjectPtr, MAddress, size_t);
extern "C" void MCC_StartLocalRegion(FrameAddress*);
extern "C" void MCC_EndLocalRegion(FrameAddress*);
extern "C" ArrayRef MCC_NewLocalObjArray(const TypeInfo*, MIndex);

namespace {
class NullGCTibMutatorScope {
public:
    NullGCTibMutatorScope()
        : savedMutator(ThreadLocal::GetMutator()), savedBuffer(ThreadLocal::GetThreadLocalData()->buffer)
    {
        ThreadLocal::GetThreadLocalData()->buffer = &alloc;
        ThreadLocal::SetMutator(&mutator);
    }
    ~NullGCTibMutatorScope()
    {
        ThreadLocal::SetMutator(savedMutator);
        ThreadLocal::GetThreadLocalData()->buffer = savedBuffer;
        alloc.ClearRegion();
    }
private:
    AllocBuffer alloc;
    Mutator mutator;
    Mutator* savedMutator;
    AllocBuffer* savedBuffer;
};
}

GC_TEST(LocalArrayCopyStruct, NullGCTibComponentCopiesPlainBytes)
{
    GcHeapFixture heap;
    NullGCTibMutatorScope binding;
    constexpr MIndex count = 4;
    constexpr size_t stride = sizeof(uintptr_t);
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
    component->SetType(TypeKind::TYPE_KIND_STRUCT);
    component->SetInstanceSize(stride);
    component->SetGCTib(GCTib{}); // reference-free element: tag 0, null StdGCTib*
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(component);
    FrameAddress frame{};
    MCC_StartLocalRegion(&frame);
    auto* source = MCC_NewLocalObjArray(arrayType, count);
    auto* destination = MCC_NewLocalObjArray(arrayType, count);
    if (source == nullptr || destination == nullptr) {
        GC_EXPECT_TRUE(source != nullptr && destination != nullptr);
        MCC_EndLocalRegion(&frame);
        return;
    }
    const GCTib tib = component->GetGCTib();
    const MSize elementSize = destination->GetElementSize();
    auto* from = reinterpret_cast<uintptr_t*>(source->ConvertToCArray());
    auto* to = reinterpret_cast<uintptr_t*>(destination->ConvertToCArray());
    for (MIndex i = 0; i < count; ++i) {
        from[i] = 0x6b00 + i;
        to[i] = 0;
    }
    CJ_MCC_ArrayCopyStruct(destination, reinterpret_cast<MAddress>(to), count * stride,
                           source, reinterpret_cast<MAddress>(from), count * stride);
    bool copied = true;
    for (MIndex i = 0; i < count; ++i) {
        copied = copied && to[i] == 0x6b00 + i && from[i] == 0x6b00 + i;
    }
    std::fprintf(stderr, "LOCAL_NULL_GCTIB_COPY_TARGET tag=%llu element=%zu copied=%d\n",
                 static_cast<unsigned long long>(tib.tag), static_cast<size_t>(elementSize), copied);
    MCC_EndLocalRegion(&frame);
    GC_EXPECT_EQ(static_cast<unsigned long long>(tib.tag), 0ull);
    GC_EXPECT_EQ(static_cast<size_t>(elementSize), stride);
    GC_EXPECT_TRUE(copied);
}
