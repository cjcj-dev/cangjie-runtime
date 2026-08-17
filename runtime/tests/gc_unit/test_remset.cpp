// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U7 — cross-gen remset: old→young write must enter RememberedSet via product Barrier path.
// Product symbols: Barrier::WriteReference → Barrier::RecordCrossGenEdge, RememberedSet::Record.
// Defect anchor: idleedge (Idle window missed bare old→young edges).
// Acceptance gate for wbclose2: after Idle barrier records, this stays green;
// if Idle WriteReferenceImpl skips MCC / RecordCrossGenEdge, Idle phase arm must go red.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <unordered_set>

#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class TestCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
    }
};

class TestBarrier final : public Barrier {
public:
    TestBarrier(Collector& collector, RememberedSet& rememberedSet) : Barrier(collector, rememberedSet) {}

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
    }
};

bool ExpectRecorded(RememberedSet& rs, MAddress fieldAddr)
{
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    return std::find(records.begin(), records.end(), fieldAddr) != records.end() && records.size() == 1;
}

} // namespace

// U7: product Barrier NVI WriteReference records old→young edge.
GC_TEST(Remset, OldToYoungRecordedByBarrier)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(rs, reinterpret_cast<MAddress>(field)));
}

// U7: product IdleBarrier WriteReference also records (wbclose2 acceptance).
// Pre-fix Idle may skip MCC; product base NVI still posts RecordCrossGenEdge after
// WriteReferenceImpl — so this is green when NVI post-record is intact.
// Red when RecordCrossGenEdge is broken or young flag missing.
GC_TEST(Remset, IdleBarrierOldToYoungRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    IdleBarrier idle(collector, rs);

    field->StoreColoured(zpointer::null);
    idle.WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(rs, reinterpret_cast<MAddress>(field)));
}

// Static roots are enumerated directly by every minor and must not enter the
// heap-only remembered set. Restoring the external record path makes this red.
GC_TEST(Remset, StaticRootNotRecorded)
{
    GcHeapFixture fx;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    RootSlot root;

    barrier.WriteStaticRef(root, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// U7: young→young must NOT enter remset (only old→young).
GC_TEST(Remset, YoungToYoungNotRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// U7: old→old must NOT enter remset.
GC_TEST(Remset, OldToOldNotRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(0);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}
