// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Mutator/Mutator.h"
#include <array>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
extern "C" void CJ_MCC_ArrayCopyRef(ObjectPtr, MAddress, size_t, ObjectPtr, MAddress, size_t);
extern "C" void CJ_MCC_ArrayCopyStruct(ObjectPtr, MAddress, size_t, ObjectPtr, MAddress, size_t);

namespace {
void CheckOverlap(bool structure, bool backwards, size_t length = 4, bool same = false)
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
    const size_t dstIndex = same ? srcIndex : backwards ? 1 : 0;
    // Diagnostic preconditions must not hide the result assertion below.
    std::fprintf(stderr, "ARRAYCOPY_PRECONDITION heap=%d length=%zu distinct=%d\n",
                 Heap::IsHeapAddress(array), static_cast<size_t>(array->GetLength()), original[0] != original[1]);
    auto copy = structure ? CJ_MCC_ArrayCopyStruct : CJ_MCC_ArrayCopyRef;
    copy(array, content + dstIndex * stride, (count - 1) * stride,
         array, content + srcIndex * stride, length * stride);
    bool referencesMatch = true;
    bool primitivesMatch = true;
    for (size_t i = 0; i < count; ++i) {
        const size_t expected = i >= dstIndex && i < dstIndex + length ? srcIndex + i - dstIndex : i;
        const MAddress destination = content + i * stride;
        BaseObject* actual = ZBarrier::ReadReference(array, HeapSlotAt<>(destination + refOffset));
        referencesMatch &= actual == original[expected];
        if (structure) {
            primitivesMatch &= *reinterpret_cast<uintptr_t*>(destination) == 100 + expected;
            primitivesMatch &= *reinterpret_cast<uintptr_t*>(destination + 2 * sizeof(uintptr_t)) == 200 + expected;
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
GC_TEST(ArrayCopyOverlap, RefSame) { CheckOverlap(false, true, 4, true); }
GC_TEST(ArrayCopyOverlap, StructSame) { CheckOverlap(true, true, 4, true); }
GC_TEST(ArrayCopyOverlap, RefEmpty) { CheckOverlap(false, true, 0); }
GC_TEST(ArrayCopyOverlap, StructEmpty) { CheckOverlap(true, true, 0); }
GC_TEST(ArrayCopyOverlap, RefDisjoint) { CheckOverlap(false, true, 1); }
GC_TEST(ArrayCopyOverlap, StructDisjoint) { CheckOverlap(true, true, 1); }

namespace {
class ArrayCopyMutatorScope {
public:
    ArrayCopyMutatorScope()
        : savedMutator(ThreadLocal::GetMutator()), savedBuffer(ThreadLocal::GetThreadLocalData()->buffer)
    {
        ThreadLocal::GetThreadLocalData()->buffer = &alloc;
        ThreadLocal::SetMutator(&mutator);
    }
    ~ArrayCopyMutatorScope()
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

void CheckBarriers(bool structure, bool checkStore)
{
    GcHeapFixture heap;
    heap.region0->reset(PageAge::old);
    ArrayCopyMutatorScope scope;
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
    component->SetType(structure ? TypeKind::TYPE_KIND_STRUCT : TypeKind::TYPE_KIND_CLASS);
    component->SetInstanceSize(sizeof(zpointer));
    GCTib bitmap{};
    bitmap.tag = SIGN_BIT | 1;
    component->SetGCTib(bitmap);
    component->SetFlagHasRefField();
    auto* type = reinterpret_cast<TypeInfo*>(arrayStorage);
    type->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    type->SetComponentTypeInfo(component);
    auto* array = reinterpret_cast<MArray*>(heap.heapStart + 256);
    *reinterpret_cast<uintptr_t*>(array) = reinterpret_cast<uintptr_t>(type);
    array->SetLength(2);
    const MAddress content = reinterpret_cast<MAddress>(array->ConvertToCArray());
    auto& source = HeapSlotAt<>(content);
    auto& destination = HeapSlotAt<>(content + sizeof(zpointer));
    const uintptr_t stale = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    const zpointer previousSource = ColouredPointer(heap.obj1, stale & (~stale + 1));
    const zpointer previousDestination = to_zpointer(raw(StoreGoodPointer(heap.obj0)) ^ ZPointerMarkedOldMask);
    source.StoreColoured(previousSource);
    destination.StoreColoured(previousDestination);
    auto copy = structure ? CJ_MCC_ArrayCopyStruct : CJ_MCC_ArrayCopyRef;
    copy(array, content + sizeof(zpointer), sizeof(zpointer), array, content, sizeof(zpointer));
    // Observe product results directly: do not run another load barrier that
    // could heal the source on behalf of the arraycopy under test.
    const bool sourceHealed = raw(source.GetFieldValue()) != raw(previousSource) &&
        ZPointer::is_load_good(source.GetFieldValue());
    const bool storedGood = raw(destination.GetFieldValue()) == raw(StoreGoodPointer(heap.obj1));
    auto* buffer = ThreadLocal::GetGCData().storeBarrierBuffer;
    const bool previousRecorded = buffer != nullptr && buffer->Pending() == 1 &&
        buffer->buffer[buffer->Current()].p == reinterpret_cast<volatile zpointer*>(&destination) &&
        raw(buffer->buffer[buffer->Current()].prev) == raw(previousDestination);
    std::fprintf(stderr, "ARRAYCOPY_BARRIER_TARGET_EXECUTED struct=%d store=%d healed=%d recorded=%d good=%d\n",
                 structure, checkStore, sourceHealed, previousRecorded, storedGood);
    if (checkStore) {
        GC_EXPECT_TRUE(previousRecorded);
    } else {
        GC_EXPECT_TRUE(sourceHealed);
    }
    GC_EXPECT_TRUE(storedGood);
}
}
GC_TEST(ArrayCopyBarrier, RefStorePreservesPrevious) { CheckBarriers(false, true); }
GC_TEST(ArrayCopyBarrier, RefLoadHealsSource) { CheckBarriers(false, false); }
GC_TEST(ArrayCopyBarrier, StructStorePreservesPrevious) { CheckBarriers(true, true); }
GC_TEST(ArrayCopyBarrier, StructLoadHealsSource) { CheckBarriers(true, false); }
