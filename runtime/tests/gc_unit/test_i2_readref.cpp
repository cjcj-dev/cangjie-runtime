// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC selects load-barrier paths from the word colour. Load-bad words resolve
// before self-healing; load-good words bypass forwarding-header inspection
// (zBarrier.inline.hpp:319-343).

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/z/zAccess.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(I2ReadRef, LoadBadForwardedFromResolvesAndHealsTo)
{
    GcHeapFixture fx;
    fx.obj0->SetStateCode(ObjectState::FORWARDED);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    const uintptr_t remap = staleRemaps & (~staleRemaps + 1);
    GC_EXPECT_TRUE(remap != 0);
    field->StoreColoured(GcUnit::ColouredPointer(fx.obj0, remap));

    BaseObject* got = HeapAccess<>::oop_load(&(*field));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(to_object(field->GetTargetObject())),
                 reinterpret_cast<uintptr_t>(fx.obj0));
}

// zBarrier.inline.hpp:322-324: a load-good colour takes the fast path;
// the forwarding state in an object header does not select the slow path.
GC_TEST(I2ReadRef, LoadGoodColourSelectsFastPath)
{
    GcHeapFixture fx;
    fx.obj0->SetStateCode(ObjectState::FORWARDED);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    // zAddress_aarch64.inline.hpp:29-31 converts logical remap state to
    // physical colour bits; ColouredPointer accepts those physical bits.
    const uintptr_t remap = ZPointerRemapped;
    const auto good = GcUnit::ColouredPointer(fx.obj0, remap);
    const bool loadGood = ZPointer::is_load_good(good);
    // Record input qualification before entering the barrier. GC_EXPECT throws,
    // so assert qualification after the original results to keep them observable
    // when deliberately reverting the input conversion.
    std::printf("I2_READREF_INPUT load_good=%d\n", loadGood);
    field.StoreColoured(good);
    GC_EXPECT_TRUE(HeapAccess<>::oop_load(&(field)) == fx.obj0);
    GC_EXPECT_EQ(raw(field.GetFieldValue()), raw(good));
    GC_EXPECT_TRUE(loadGood);
}

GC_TEST(I2ReadRef, LoadBadHeapSlotIsHealedToCurrentColour)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t stale = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    GC_EXPECT_TRUE(stale != 0);
    const auto previous = GcUnit::ColouredPointer(fx.obj0, stale & (~stale + 1));
    field->StoreColoured(previous);

    BaseObject* got = HeapAccess<>::oop_load(&(*field));
    GC_EXPECT_TRUE(got == fx.obj0);
    GC_EXPECT_TRUE(ClassifySlotWord(static_cast<uintptr_t>(raw(field->GetFieldValue()))) ==
                   SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(static_cast<uintptr_t>(raw(field->GetFieldValue())) != raw(previous));
}
