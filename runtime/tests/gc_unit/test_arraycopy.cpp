// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/FieldInfo.h"
#include "ObjectModel/MObject.inline.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Mutator/Mutator.h"
#include <array>

#include "Heap/z/zAccess.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
extern "C" void MCC_WriteStructField(ObjectPtr, MAddress, size_t, MAddress, size_t, GCTib);
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
    heap.region0()->SetRegionAllocPtr(heap.heapStart + 2048);
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
        BaseObject* actual = HeapAccess<>::oop_load(&(HeapSlotAt<>(destination + refOffset)));
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
    heap.region0()->reset(PageAge::old);
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

// Header-only template arm: the product template itself supplies the slot bits.
GC_TEST(AccessBarrier976, ClearOnePublishesColorNull)
{
    GcHeapFixture fx;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    ZBarrierSet::AccessBarrier<IN_HEAP | ON_STRONG_OOP_REF>::oop_clear_one(
        reinterpret_cast<volatile zpointer*>(&field));
    std::fprintf(stderr, "ACCESS976_CLEAR_ASSERT actual=%zx color_null=%zx store_good_null=%zx\n",
        raw(field.GetFieldValue()), raw(color_null()), raw(ZAddress::store_good(zaddress::null)));
    GC_EXPECT_EQ(field.GetFieldValue(), color_null());
    GC_EXPECT_TRUE(color_null() != ZAddress::store_good(zaddress::null));
}

namespace {
enum class BulkAccess976 { Slots, RefArray, Value, ValueArray };
template<bool rawAccess>
void CheckBulkBits976(BulkAccess976 kind)
{
    GcHeapFixture heap;
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
    component->SetType(TypeKind::TYPE_KIND_STRUCT);
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
    auto* source = reinterpret_cast<zpointer*>(array->ConvertToCArray());
    auto* destination = source + 1;
    *source = color_null();
    *destination = StoreGoodPointer(heap.obj0);
    using Api = Access<IN_HEAP | (rawAccess ? AS_RAW : DECORATORS_NONE)>;
    switch (kind) {
        case BulkAccess976::Slots:
            Api::oop_arraycopy(source, destination, 1);
            break;
        case BulkAccess976::RefArray:
            Api::oop_arraycopy(array, reinterpret_cast<MAddress>(source), sizeof(zpointer),
                              array, reinterpret_cast<MAddress>(destination), sizeof(zpointer));
            break;
        case BulkAccess976::Value:
            Api::value_copy(ValuePayload(reinterpret_cast<MAddress>(source), sizeof(zpointer),
                                        std::vector<size_t>{0}, ValuePayload::Kind::Heap),
                            ValuePayload(reinterpret_cast<MAddress>(destination), sizeof(zpointer), ValuePayload::Kind::Heap));
            break;
        case BulkAccess976::ValueArray:
            Api::value_arraycopy(array, reinterpret_cast<MAddress>(source), sizeof(zpointer),
                                array, reinterpret_cast<MAddress>(destination), sizeof(zpointer));
            break;
    }
    const zpointer expected = rawAccess ? color_null() : ZAddress::store_good(zaddress::null);
    std::fprintf(stderr, "ACCESS976_BULK_ASSERT raw=%d api=%d actual=%zx expected=%zx\n",
                 rawAccess, static_cast<int>(kind), raw(*destination), raw(expected));
    GC_EXPECT_EQ(raw(*destination), raw(expected));
    GC_EXPECT_TRUE(color_null() != ZAddress::store_good(zaddress::null));
}
}
GC_TEST(AccessBarrier976, RawArrayCopyPreservesBits) { CheckBulkBits976<true>(BulkAccess976::Slots); }
GC_TEST(AccessBarrier976, RawRefArrayCopyPreservesBits) { CheckBulkBits976<true>(BulkAccess976::RefArray); }
GC_TEST(AccessBarrier976, RawValueCopyPreservesBits) { CheckBulkBits976<true>(BulkAccess976::Value); }
GC_TEST(AccessBarrier976, RawValueArrayCopyPreservesBits) { CheckBulkBits976<true>(BulkAccess976::ValueArray); }
GC_TEST(AccessBarrier976, NormalArrayCopyColorsControl) { CheckBulkBits976<false>(BulkAccess976::Slots); }
GC_TEST(AccessBarrier976, NormalRefArrayCopyColorsControl) { CheckBulkBits976<false>(BulkAccess976::RefArray); }
GC_TEST(AccessBarrier976, NormalValueCopyColorsControl) { CheckBulkBits976<false>(BulkAccess976::Value); }
GC_TEST(AccessBarrier976, NormalValueArrayCopyColorsControl) { CheckBulkBits976<false>(BulkAccess976::ValueArray); }
GC_TEST(AccessBarrier976, RawScalarPreservesZeroControl)
{
    GcHeapFixture heap;
    zpointer source = to_zpointer(0);
    zpointer destination = color_null();
    RawAccess<>::oop_store(&destination, RawAccess<>::oop_load(&source));
    std::fprintf(stderr, "ACCESS976_SCALAR_ASSERT actual=%zx expected=0\n", raw(destination));
    GC_EXPECT_EQ(raw(destination), uintptr_t(0));
}
GC_TEST(AccessBarrier976, RawArrayCopyOverlapAndEmpty)
{
    for (bool backwards : {false, true}) {
        zpointer slots[] = {to_zpointer(1), to_zpointer(2), to_zpointer(3), to_zpointer(4)};
        RawAccess<>::oop_arraycopy(slots + !backwards, slots + backwards, 3);
        for (size_t i = 0; i < 3; ++i) {
            GC_EXPECT_EQ(raw(slots[i + backwards]), i + 1 + !backwards);
        }
        const auto before = slots[0];
        RawAccess<>::oop_arraycopy(slots, slots + 1, 0);
        RawAccess<>::oop_arraycopy(slots, slots, 4);
        GC_EXPECT_EQ(slots[0], before);
    }
}
GC_TEST(AccessBarrier976, RawValueCopyAlignedSegmentsAndTail)
{
    alignas(8) unsigned char source[32];
    alignas(8) unsigned char destination[32];
    for (size_t offset : {0, 4, 2, 1}) {
        for (size_t i = 0; i < sizeof(source); ++i) { source[i] = i + 1; destination[i] = 0; }
        RawAccess<>::value_copy(ValuePayload(reinterpret_cast<MAddress>(source + offset), 15,
                                            ValuePayload::Kind::Uncolored),
                               ValuePayload(reinterpret_cast<MAddress>(destination + offset), 15,
                                            ValuePayload::Kind::Uncolored));
        GC_EXPECT_EQ(std::memcmp(source + offset, destination + offset, 15), 0);
        GC_EXPECT_EQ(destination[offset + 15], 0);
    }
}

namespace {
// Exercise the compiler ABI; read the payload produced by the product SO.
// These assertions establish copy wiring, not a concurrency/atomicity proof.
void CheckPrimitivePayload976(bool references, bool trailer)
{
    GcHeapFixture heap;
    alignas(8) unsigned char source[32]{};
    const MAddress destination = heap.heapStart + 2048;
    constexpr size_t size = 31; // long/int/short segments and a byte tail
    std::memset(reinterpret_cast<void*>(destination), 0, 32);
    for (size_t i = 0; i < size; ++i) { source[i] = static_cast<unsigned char>(i + 1); }
    GCTib bitmap{};
    bitmap.tag = SIGN_BIT | (references ? 2 : 0); // primitive, reference, primitive
    if (references) {
        *reinterpret_cast<BaseObject**>(source + 8) = heap.obj0;
        HeapSlotAt<>(destination + 8).StoreColoured(StoreGoodPointer(nullptr));
    }
    MCC_WriteStructField(heap.obj0, destination, size,
                         reinterpret_cast<MAddress>(source), size, bitmap);
    const size_t begin = references && trailer ? 16 : 0;
    const size_t end = references && !trailer ? 8 : size;
    const bool payloadMatches = std::memcmp(reinterpret_cast<void*>(destination + begin),
                                             source + begin, end - begin) == 0;
    std::fprintf(stderr, "VALUE976_PAYLOAD_TARGET refs=%d begin=%zu end=%zu matches=%d\n",
                 references, begin, end, payloadMatches);
    GC_EXPECT_TRUE(payloadMatches);
    GC_EXPECT_EQ(*reinterpret_cast<unsigned char*>(destination + size), 0);
    if (references) {
        GC_EXPECT_TRUE(HeapAccess<>::oop_load(&(HeapSlotAt<>(destination + 8))) == heap.obj0);
    }
}
}
GC_TEST(AccessBarrier976, PrimitiveValueNoReferences) { CheckPrimitivePayload976(false, false); }
GC_TEST(AccessBarrier976, PrimitiveValueLeadingGap) { CheckPrimitivePayload976(true, false); }
GC_TEST(AccessBarrier976, PrimitiveValueTrailer) { CheckPrimitivePayload976(true, true); }

// ZGC zBarrierSet.inline.hpp:477-479 obtains the oop layout from the source value.
extern "C" void MCC_SetInstanceFieldValue(InstanceFieldInfo*, TypeInfo*, ObjRef, ObjRef);
namespace {
void CheckReflectionSourceLayout(bool aggregate)
{
    GcHeapFixture heap;
    BaseObject* source = heap.PlaceObject(heap.heapStart + 128);
    alignas(TypeInfo) unsigned char sourceTypeStorage[sizeof(TypeInfo)]{};
    auto* sourceType = reinterpret_cast<TypeInfo*>(sourceTypeStorage);
    sourceType->SetType(aggregate ? TypeKind::TYPE_KIND_STRUCT : TypeKind::TYPE_KIND_CLASS);
    sourceType->SetInstanceSize(sizeof(uintptr_t));
    sourceType->SetFlagHasRefField();
    GCTib sourceLayout{};
    sourceLayout.tag = SIGN_BIT | 1; // source: reference
    sourceType->SetGCTib(sourceLayout);
    *reinterpret_cast<uintptr_t*>(source) = reinterpret_cast<uintptr_t>(sourceType);

    GCTib containerLayout{};
    containerLayout.tag = SIGN_BIT | 2; // container: primitive, reference
    heap.typeInfo->SetGCTib(containerLayout);
    heap.typeInfo->SetInstanceSize(2 * sizeof(uintptr_t));
    TypeInfo* fieldTypes[] = {sourceType};
    U32 offsets[] = {sizeof(uintptr_t)};
    heap.typeInfo->SetFieldNum(1);
    heap.typeInfo->SetFieldAddr(fieldTypes);
    heap.typeInfo->SetOffsets(offsets);
    auto& sourceSlot = HeapSlotAt<>(reinterpret_cast<MAddress>(source) + TYPEINFO_PTR_SIZE);
    auto& destination = HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj0) + TYPEINFO_PTR_SIZE + sizeof(uintptr_t));
    sourceSlot.StoreColoured(StoreGoodPointer(source));
    destination.StoreColoured(StoreGoodPointer(heap.obj0));
    InstanceFieldInfo field{};
    MCC_SetInstanceFieldValue(&field, heap.typeInfo, static_cast<ObjRef>(heap.obj0), static_cast<ObjRef>(source));
    const zpointer actual = destination.GetFieldValue();
    const zpointer expected = StoreGoodPointer(source);
    std::fprintf(stderr, "REFLECTION_LAYOUT_TARGET aggregate=%d actual=%lx expected=%lx\n",
                 aggregate, raw(actual), raw(expected));
    GC_EXPECT_EQ(actual, expected);
}
}
GC_TEST(AccessBarrier976, ReflectionAggregateSourceLayout) { CheckReflectionSourceLayout(true); }
GC_TEST(AccessBarrier976, ReflectionReferenceLayoutControl) { CheckReflectionSourceLayout(false); }
