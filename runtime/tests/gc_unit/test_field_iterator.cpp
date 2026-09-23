// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Common/BaseObject.inline.h"
#include "Heap/z/zIterator.inline.hpp"
#include <cstdio>
#include <vector>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct Layout {
    alignas(TypeInfo) unsigned char typeStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)]{};
    alignas(BaseObject) unsigned char storage[1024]{};
    TypeInfo* type = reinterpret_cast<TypeInfo*>(typeStorage);
    TypeInfo* component = reinterpret_cast<TypeInfo*>(componentStorage);
    BaseObject* object = reinterpret_cast<BaseObject*>(storage);

    Layout()
    {
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetFlagHasRefField();
        type->SetInstanceSize(128 * sizeof(void*));
        GCTib tib{};
        tib.tag = SIGN_BIT | 0x25; // Slots 0, 2 and 5, in address order.
        type->SetGCTib(tib);
        object->SetClassInfo(type);
    }

    MAddress payload() const { return reinterpret_cast<MAddress>(storage) + TYPEINFO_PTR_SIZE; }

    MArray* array(bool record)
    {
        component->SetType(record ? TypeKind::TYPE_KIND_STRUCT : TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(3 * sizeof(void*));
        GCTib tib{};
        tib.tag = SIGN_BIT | 5;
        component->SetGCTib(tib);
        component->SetFlagHasRefField();
        type->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        type->SetComponentTypeInfo(component);
        auto* result = reinterpret_cast<MArray*>(object);
        result->SetLength(3);
        return result;
    }
};

// A concrete, non-copyable functor cannot be stored in std::function. The
// runtime assertions below separately validate the product template results.
struct Slots {
    std::vector<MAddress>& output;
    explicit Slots(std::vector<MAddress>& output) : output(output) {}
    Slots(const Slots&) = delete;
    void operator()(HeapSlot<>& slot) const { output.push_back(reinterpret_cast<MAddress>(&slot)); }
};

void ExpectSlots(const char* path, const std::vector<MAddress>& actual,
                 MAddress base, std::initializer_list<size_t> indices)
{
    std::vector<MAddress> expected;
    for (size_t index : indices) { expected.push_back(base + index * sizeof(HeapSlot<>)); }
    std::fprintf(stderr, "FIELD_ITERATOR_RESULT path=%s actual=%zu expected=%zu exact=%d\n",
                 path, actual.size(), expected.size(), actual == expected);
    GC_EXPECT_TRUE(actual == expected);
}
}

namespace {
template <OopIterateClosure::ReferenceIterationMode Mode>
class ReferenceFieldsClosure : public BasicOopIterateClosure {
    std::vector<MAddress>& fields;
public:
    explicit ReferenceFieldsClosure(std::vector<MAddress>& fields) : fields(fields) {}
    ReferenceIterationMode reference_iteration_mode() override { return Mode; }
    void do_oop(HeapSlot<>* field) override { fields.push_back(reinterpret_cast<MAddress>(field)); }
};
}

// HotSpot instanceRefKlass.inline.hpp:107-124: the ordinary fields and
// referent are distinct, and null discovery falls through to fields.
GC_TEST(FieldIterator, ReferenceFieldsIncludesReferent)
{
    Layout layout;
    layout.type->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    std::vector<MAddress> actual;
    ReferenceFieldsClosure<OopIterateClosure::DO_FIELDS> closure(actual);
    ZIterator::oop_iterate(layout.object, &closure);
    ExpectSlots("reference-fields", actual, layout.payload(), {2, 5, 0});
}

GC_TEST(FieldIterator, ReferenceFieldsExceptReferent)
{
    Layout layout;
    layout.type->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    std::vector<MAddress> actual;
    ReferenceFieldsClosure<OopIterateClosure::DO_FIELDS_EXCEPT_REFERENT> closure(actual);
    ZIterator::oop_iterate(layout.object, &closure);
    ExpectSlots("reference-fields-except", actual, layout.payload(), {2, 5});
}

GC_TEST(FieldIterator, NullDiscovererUsesFields)
{
    Layout layout;
    layout.type->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    std::vector<MAddress> actual;
    auto visitor = [&](HeapSlot<>& field) { actual.push_back(reinterpret_cast<MAddress>(&field)); };
    ZBasicOopIterateClosure<decltype(visitor)> closure(visitor);
    GC_EXPECT_TRUE(closure.ref_discoverer() == nullptr);
    GC_EXPECT_EQ(closure.reference_iteration_mode(), OopIterateClosure::DO_DISCOVERY);
    ZIterator::oop_iterate(layout.object, &closure);
    ExpectSlots("null-discoverer", actual, layout.payload(), {2, 5, 0});
}

GC_TEST(FieldIterator, ShortBitmapConcreteVisitor)
{
    Layout layout;
    std::vector<MAddress> actual;
    layout.object->ForEachRefField(Slots(actual));
    ExpectSlots("short", actual, layout.payload(), {0, 2, 5});
}

GC_TEST(FieldIterator, StandardBitmapConcreteVisitor)
{
    Layout layout;
    struct { U32 count; U8 words[9]; } bitmap{9, {0x81, 0, 0, 0, 0, 0, 0, 0x40, 2}};
    GCTib tib{};
    tib.gctib = reinterpret_cast<StdGCTib*>(&bitmap);
    layout.type->SetGCTib(tib);
    std::vector<MAddress> actual;
    layout.object->ForEachRefField(Slots(actual));
    ExpectSlots("standard", actual, layout.payload(), {0, 7, 62, 65});
}

GC_TEST(FieldIterator, ExplicitKlassDispatch)
{
    Layout layout;
    layout.component->SetType(TypeKind::TYPE_KIND_CLASS);
    layout.component->SetFlagHasRefField();
    GCTib tib{};
    tib.tag = SIGN_BIT | 2;
    layout.component->SetGCTib(tib);
    std::vector<MAddress> actual;
    auto visitor = [&](HeapSlot<>& slot) { actual.push_back(reinterpret_cast<MAddress>(&slot)); };
    ZIterator::basic_oop_iterate_safe(layout.object, layout.component, visitor);
    ExpectSlots("explicit-klass", actual, layout.payload(), {1});
}

GC_TEST(FieldIterator, ReferenceArrayFullAndRange)
{
    Layout layout;
    MArray* array = layout.array(false);
    MAddress first = reinterpret_cast<MAddress>(array->ConvertToCArray());
    std::vector<MAddress> actual;
    array->ForEachRefField(Slots(actual));
    ExpectSlots("reference-array-full", actual, first, {0, 1, 2});
    actual.clear();
    auto visitor = [&](HeapSlot<>& slot) { actual.push_back(reinterpret_cast<MAddress>(&slot)); };
    ZBasicOopIterateClosure<decltype(visitor)> closure(visitor);
    ZIterator::oop_iterate_elements_range(array, &closure, 1, 3);
    ExpectSlots("reference-array-range", actual, first, {1, 2});
}

GC_TEST(FieldIterator, RecordArrayFullAndRange)
{
    Layout layout;
    MArray* array = layout.array(true);
    MAddress first = reinterpret_cast<MAddress>(array->ConvertToCArray());
    std::vector<MAddress> actual;
    array->ForEachRefField(Slots(actual));
    ExpectSlots("record-array-full", actual, first, {0, 2, 3, 5, 6, 8});
    actual.clear();
    array->ForEachRefFieldInRange(Slots(actual), first + 3 * sizeof(void*), first + 6 * sizeof(void*));
    ExpectSlots("record-array-range", actual, first, {3, 5});
    actual.clear();
    array->ForEachRefInStruct(Slots(actual), first + 4 * sizeof(void*), first + 6 * sizeof(void*));
    ExpectSlots("record-array-aggregate", actual, first, {5});
}

GC_TEST(FieldIterator, ObjectAggregateShortAndStandard)
{
    Layout layout;
    std::vector<MAddress> actual;
    MAddress first = layout.payload();
    layout.object->ForEachRefInStruct(Slots(actual), first + sizeof(void*), first + 5 * sizeof(void*));
    ExpectSlots("short-aggregate", actual, first, {2});
    struct { U32 count; U8 words[2]; } bitmap{2, {0x81, 2}};
    GCTib tib{};
    tib.gctib = reinterpret_cast<StdGCTib*>(&bitmap);
    layout.type->SetGCTib(tib);
    actual.clear();
    layout.object->ForEachRefInStruct(Slots(actual), first + sizeof(void*), first + 10 * sizeof(void*));
    ExpectSlots("standard-aggregate", actual, first, {7, 9});
}

GC_TEST(FieldIterator, InvisibleReferenceArraySafeBoundary)
{
    Layout layout;
    MArray* array = layout.array(false);
    std::vector<MAddress> actual;
    auto visitor = [&](HeapSlot<>& slot) { actual.push_back(reinterpret_cast<MAddress>(&slot)); };
    array->SetInvisibleObject(true);
    ZIterator::basic_oop_iterate_safe(array, visitor);
    ExpectSlots("invisible-safe", actual, reinterpret_cast<MAddress>(array->ConvertToCArray()), {});
    array->SetInvisibleObject(false);
    ZIterator::basic_oop_iterate_safe(array, visitor);
    ExpectSlots("visible-positive-control", actual, reinterpret_cast<MAddress>(array->ConvertToCArray()), {0, 1, 2});
}
