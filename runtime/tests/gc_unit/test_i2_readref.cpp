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

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class ToCollector final : public Collector {
public:
    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject* obj, Generation) const override
    {
        return obj == from ? FindToVersionResult::Found(to) : FindToVersionResult::NotForwarded();
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId) const override
    {
        return obj == from ? to : obj;
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        const uintptr_t remap = ZPointerRemapped;
        return RefField<>(GcUnit::ColouredPointer(obj, remap));
    }
};

} // namespace

GC_TEST(I2ReadRef, LoadBadForwardedFromResolvesAndHealsTo)
{
    GcHeapFixture fx;
    ToCollector collector;
    collector.from = fx.obj0;
    collector.to = fx.obj1;
    fx.obj0->SetStateCode(ObjectState::FORWARDED);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    const uintptr_t remap = staleRemaps & (~staleRemaps + 1);
    GC_EXPECT_TRUE(remap != 0);
    field->StoreColoured(GcUnit::ColouredPointer(fx.obj0, remap));

    BaseObject* got = ZBarrier::ReadReference(fx.obj0, *field);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(to_object(field->GetTargetObject())),
                 reinterpret_cast<uintptr_t>(fx.obj0));
}

// zBarrier.inline.hpp:322-324: a load-good colour takes the fast path;
// the forwarding state in an object header does not select the slow path.
GC_TEST(I2ReadRef, LoadGoodColourSelectsFastPath)
{
    GcHeapFixture fx;
    ToCollector collector;
    collector.from = fx.obj0;
    collector.to = fx.obj1;
    fx.obj0->SetStateCode(ObjectState::FORWARDED);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t remap = ZPointerRemapped;
    const auto good = GcUnit::ColouredPointer(fx.obj0, remap);
    field.StoreColoured(good);
    GC_EXPECT_TRUE(ZBarrier::ReadReference(fx.obj1, field) == fx.obj0);
    GC_EXPECT_EQ(raw(field.GetFieldValue()), raw(good));
}

GC_TEST(I2ReadRef, LoadBadHeapSlotIsHealedToCurrentColour)
{
    GcHeapFixture fx;
    ToCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t stale = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
    GC_EXPECT_TRUE(stale != 0);
    const auto previous = GcUnit::ColouredPointer(fx.obj0, stale & (~stale + 1));
    field->StoreColoured(previous);

    BaseObject* got = ZBarrier::ReadReference(fx.obj1, *field);
    GC_EXPECT_TRUE(got == fx.obj0);
    GC_EXPECT_TRUE(ClassifySlotWord(static_cast<uintptr_t>(raw(field->GetFieldValue()))) ==
                   SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(static_cast<uintptr_t>(raw(field->GetFieldValue())) != raw(previous));
}
