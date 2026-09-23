// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include <array>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
extern "C" void CJ_MCC_ArrayCopyRef(ObjectPtr, MAddress, size_t, ObjectPtr, MAddress, size_t);
extern "C" void CJ_MCC_ArrayCopyStruct(ObjectPtr, MAddress, size_t, ObjectPtr, MAddress, size_t);

namespace {
void CheckOverlap(bool structure, bool backwards)
{
    GcHeapFixture heap;
    constexpr size_t count = 5;
    const size_t stride = (structure ? 3 : 1) * sizeof(uintptr_t);
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
    component->SetType(structure ? TypeKind::TYPE_KIND_STRUCT : TypeKind::TYPE_KIND_CLASS);
    component->SetInstanceSize(stride);
    if (structure) {
        component->SetFlagHasRefField();
        GCTib bitmap{};
        bitmap.tag = SIGN_BIT | 2; // primitive, reference, primitive
        component->SetGCTib(bitmap);
    }
    auto* type = reinterpret_cast<TypeInfo*>(arrayStorage);
    type->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    type->SetComponentTypeInfo(component);
    auto* array = reinterpret_cast<MArray*>(heap.heapStart + 256);
    *reinterpret_cast<uintptr_t*>(array) = reinterpret_cast<uintptr_t>(type);
    array->SetLength(count);
    const MAddress content = reinterpret_cast<MAddress>(array->ConvertToCArray());
    const size_t refOffset = structure ? sizeof(uintptr_t) : 0;
    std::array<BaseObject*, count> original{};
    for (size_t i = 0; i < count; ++i) {
        original[i] = heap.PlaceObject(heap.heapStart + 1024 + i * 32);
        if (structure) {
            *reinterpret_cast<uintptr_t*>(content + i * stride) = 100 + i;
            *reinterpret_cast<uintptr_t*>(content + i * stride + 2 * sizeof(uintptr_t)) = 200 + i;
        }
        HeapSlotAt<>(content + i * stride + refOffset).StoreColoured(StoreGoodPointer(original[i]));
    }
    heap.region0->SetRegionAllocPtr(heap.heapStart + 2048);
    const size_t srcIndex = backwards ? 0 : 1;
    const size_t dstIndex = backwards ? 1 : 0;
    // Diagnostic preconditions must not hide the result assertion below.
    std::fprintf(stderr, "ARRAYCOPY_PRECONDITION heap=%d length=%zu distinct=%d\n",
                 Heap::IsHeapAddress(array), static_cast<size_t>(array->GetLength()), original[0] != original[1]);
    auto copy = structure ? CJ_MCC_ArrayCopyStruct : CJ_MCC_ArrayCopyRef;
    copy(array, content + dstIndex * stride, (count - 1) * stride,
         array, content + srcIndex * stride, (count - 1) * stride);
    bool referencesMatch = true;
    bool primitivesMatch = true;
    for (size_t i = 0; i < count - 1; ++i) {
        const MAddress destination = content + (dstIndex + i) * stride;
        BaseObject* actual = ZBarrier::ReadReference(array, HeapSlotAt<>(destination + refOffset));
        referencesMatch &= actual == original[srcIndex + i];
        if (structure) {
            primitivesMatch &= *reinterpret_cast<uintptr_t*>(destination) == 100 + srcIndex + i;
            primitivesMatch &= *reinterpret_cast<uintptr_t*>(destination + 2 * sizeof(uintptr_t)) == 200 + srcIndex + i;
        }
    }
    std::fprintf(stderr, "ARRAYCOPY_TARGET_EXECUTED struct=%d backwards=%d refs=%d primitives=%d\n",
                 structure, backwards, referencesMatch, primitivesMatch);
    GC_EXPECT_TRUE(referencesMatch);
    GC_EXPECT_TRUE(primitivesMatch);
}
}
GC_TEST(ArrayCopyOverlap, RefBackward) { CheckOverlap(false, true); }
GC_TEST(ArrayCopyOverlap, RefForward) { CheckOverlap(false, false); }
GC_TEST(ArrayCopyOverlap, StructBackward) { CheckOverlap(true, true); }
GC_TEST(ArrayCopyOverlap, StructForward) { CheckOverlap(true, false); }
