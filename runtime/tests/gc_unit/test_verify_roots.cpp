// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_verify_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zVerify.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/RefField.inline.h"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
// Plant the raw stack-map word, including deliberately invalid test inputs.
void SetRootWord(RootSlot& slot, uintptr_t word) { std::memcpy(&slot, &word, sizeof(word)); }
}
// zVerify.cpp:119-128: positive counterpart to the invalid-address cases.
GC_OTHER_VM_TEST(ZVerify, AcceptsActualObjectAddress)
{
    GcVerifyFixture fixture;
    ZVerify::Object(fixture.obj0, &fixture.obj0);
    GC_EXPECT_TRUE(fixture.obj0->IsValidObject());
}

// zVerify.cpp:247,323 and zHeapIterator.cpp:145 consume heap-oop slots.
// Cangjie stack maps also name stack objects and headerless ABI records.
GC_OTHER_VM_TEST(ZVerify, StackRootExpandsToActualHeapSlot)
{
    GcVerifyFixture fixture;
    alignas(16) uintptr_t storage[8] {};
    auto* object = reinterpret_cast<BaseObject*>(&storage[2]);
    object->SetClassInfo(fixture.typeInfo);
    RootSlot& record = RootSlotAt(static_cast<void*>(&storage[6]));
    StorePlain(record, from_object(fixture.obj0));
    SetRootWord(RootSlotAt(static_cast<void*>(&storage[3])), reinterpret_cast<uintptr_t>(&storage[6]));
    Mutator mutator;
    mutator.SetStackTopAddr(reinterpret_cast<uintptr_t>(storage));
    mutator.SetStackSize(sizeof(storage));
    RootSlot root;
    SetRootWord(root, reinterpret_cast<uintptr_t>(object));
    size_t visits = 0;
    mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
        ++visits;
        GC_EXPECT_TRUE(&slot == &record);
        GC_EXPECT_EQ(raw(slot.LoadPlain()), reinterpret_cast<uintptr_t>(fixture.obj0));
        ZVerify::Object(reinterpret_cast<BaseObject*>(raw(slot.LoadPlain())), &slot);
    });
    GC_EXPECT_EQ(visits, size_t(1));
    std::fprintf(stderr, "STACK_HEAP_SLOT_ASSERT_EXECUTED visits=%zu\n", visits);
}

GC_OTHER_VM_TEST(ZVerify, StackRootCycleTerminatesWithoutEmittingStackObject)
{
    GcVerifyFixture fixture;
    alignas(16) uintptr_t storage[6] {};
    auto* object = reinterpret_cast<BaseObject*>(&storage[2]);
    object->SetClassInfo(fixture.typeInfo);
    SetRootWord(RootSlotAt(static_cast<void*>(&storage[3])), reinterpret_cast<uintptr_t>(object));
    Mutator mutator;
    mutator.SetStackTopAddr(reinterpret_cast<uintptr_t>(storage));
    mutator.SetStackSize(sizeof(storage));
    RootSlot root;
    SetRootWord(root, reinterpret_cast<uintptr_t>(object));
    size_t visits = 0;
    mutator.VisitHeapRootSlots(root, [&](ObjectRef&) { ++visits; });
    GC_EXPECT_EQ(visits, size_t(0));
    // The same adapter must still deliver a real heap root.
    StorePlain(root, from_object(fixture.obj0));
    mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
        ++visits;
        GC_EXPECT_TRUE(&slot == &root);
    });
    GC_EXPECT_EQ(visits, size_t(1));
}

GC_OTHER_VM_TEST(ZVerify, RootAdapterPreservesInvalidNonStackAddress)
{
    Mutator mutator;
    RootSlot root;
    SetRootWord(root, 0x1000);
    size_t visits = 0;
    mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
        ++visits;
        GC_EXPECT_TRUE(&slot == &root);
        GC_EXPECT_EQ(raw(slot.LoadPlain()), uintptr_t(0x1000));
    });
    GC_EXPECT_EQ(visits, size_t(1));
}
