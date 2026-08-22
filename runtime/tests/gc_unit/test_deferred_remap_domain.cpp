// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <atomic>

#include "Heap/Collector/DeferredRemapDomain.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace DRD = MapleRuntime::DeferredRemapDomain;

namespace {

DRD::Record MakeRecord(GcHeapFixture& fixture, MAddress slot, BaseObject* target)
{
    RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
    RegionInfo* from = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    DRD::Record record;
    record.slot = slot;
    record.holderStart = holder->GetRegionStart();
    record.holderLife = holder->GetRegionLifeId();
    record.targetFrom = reinterpret_cast<MAddress>(target);
    record.targetFromStart = from->GetRegionStart();
    record.targetFromLife = from->GetRegionLifeId();
    record.producer = DRD::Producer::OldMark;
    (void)fixture;
    return record;
}

void MakeRetainable(RegionInfo* region)
{
    ZForwardingLife::ResetForForwarding(region->metadata.fwdRefCount, region->metadata.fwdClaimed,
                                        region->metadata.fwdDone);
}

} // namespace

GC_TEST(DeferredRemapDomain, DoubleFaceShadowParityCompletes)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    DRD::Record record = MakeRecord(fixture, slot, fixture.obj1);

    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    GC_EXPECT_FALSE(DRD::InsertForTesting(record));
    DRD::FlipForYoung(1);
    GC_EXPECT_EQ(DRD::ConsumePrevious([](const DRD::Record&) {
        return DRD::ConsumeResult::SimulatedHeal;
    }), 1u);
    GC_EXPECT_EQ(DRD::GetSnapshot().awaitingOracle, 1u);

    DRD::BeginPostflip(1);
    DRD::NotePostflipSlot(slot, fixture.obj0, fixture.obj1, 1, true);
    DRD::EndPostflip();
    const DRD::Snapshot snapshot = DRD::GetSnapshot();
    GC_EXPECT_EQ(snapshot.current, 0u);
    GC_EXPECT_EQ(snapshot.previous, 0u);
    GC_EXPECT_EQ(snapshot.awaitingOracle, 0u);
    // Same-slot producers coalesce latest-wins; this is not a duplicate
    // obligation in the parity ledger.
    GC_EXPECT_EQ(snapshot.duplicate, 0u);
    GC_EXPECT_EQ(snapshot.missing, 0u);
    GC_EXPECT_EQ(snapshot.extra, 0u);
}

GC_TEST(DeferredRemapDomain, OracleMissingIsCounted)
{
    GcHeapFixture fixture;
    DRD::ResetForTesting(8);
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    DRD::BeginPostflip(2);
    DRD::NotePostflipSlot(slot, fixture.obj0, fixture.obj1, 2, true);
    DRD::EndPostflip();
    GC_EXPECT_EQ(DRD::GetSnapshot().missing, 1u);
}

GC_TEST(DeferredRemapDomain, UnfixedShadowEntryRequeues)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    DRD::Record record = MakeRecord(
        fixture, reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE, fixture.obj1);
    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    DRD::FlipForYoung(3);
    (void)DRD::ConsumePrevious([](const DRD::Record&) {
        return DRD::ConsumeResult::SimulatedHeal;
    });
    DRD::BeginPostflip(3);
    DRD::EndPostflip();
    const DRD::Snapshot snapshot = DRD::GetSnapshot();
    GC_EXPECT_EQ(snapshot.extra, 1u);
    GC_EXPECT_EQ(snapshot.requeue, 1u);
    GC_EXPECT_EQ(snapshot.current, 1u);
}

GC_TEST(DeferredRemapDomain, LifeMismatchNeverCompletes)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    DRD::Record record = MakeRecord(
        fixture, reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE, fixture.obj1);
    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    fixture.region0->metadata.regionLifeId.fetch_add(1, std::memory_order_release);
    DRD::FlipForYoung(4);
    bool called = false;
    (void)DRD::ConsumePrevious([&called](const DRD::Record&) {
        called = true;
        return DRD::ConsumeResult::Complete;
    });
    const DRD::Snapshot snapshot = DRD::GetSnapshot();
    GC_EXPECT_FALSE(called);
    GC_EXPECT_EQ(snapshot.lifeMismatch, 1u);
    GC_EXPECT_EQ(snapshot.requeue, 1u);
    GC_EXPECT_EQ(snapshot.current, 1u);
}

GC_TEST(DeferredRemapDomain, DeadHolderDischarges)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    DRD::Record record = MakeRecord(
        fixture, reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE, fixture.obj1);
    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    fixture.region0->SetRegionType(RegionInfo::RegionType::GARBAGE_REGION);
    DRD::FlipForYoung(5);
    bool called = false;
    (void)DRD::ConsumePrevious([&called](const DRD::Record&) {
        called = true;
        return DRD::ConsumeResult::Complete;
    });
    const DRD::Snapshot snapshot = DRD::GetSnapshot();
    GC_EXPECT_FALSE(called);
    GC_EXPECT_EQ(snapshot.holderDead, 1u);
    GC_EXPECT_EQ(snapshot.consumed, 1u);
    GC_EXPECT_EQ(snapshot.current, 0u);
}

GC_TEST(DeferredRemapDomain, HolderMoveUsesExactFieldOffset)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    const MAddress fromBase = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress toBase = reinterpret_cast<MAddress>(fixture.obj1);
    const MAddress fromSlot = fromBase + TYPEINFO_PTR_SIZE;
    DRD::Record record = MakeRecord(fixture, fromSlot, fixture.obj1);
    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    GC_EXPECT_EQ(DRD::TransferObjectSlots(fromBase, toBase, 64), 1u);
    DRD::FlipForYoung(7);
    MAddress consumedSlot = 0;
    RegionLifeId consumedLife = 0;
    (void)DRD::ConsumePrevious([&](const DRD::Record& moved) {
        consumedSlot = moved.slot;
        consumedLife = moved.holderLife;
        return DRD::ConsumeResult::Complete;
    });
    GC_EXPECT_EQ(consumedSlot, toBase + TYPEINFO_PTR_SIZE);
    GC_EXPECT_EQ(consumedLife, fixture.region1->GetRegionLifeId());
    GC_EXPECT_EQ(DRD::GetSnapshot().capturedByProducer[static_cast<size_t>(DRD::Producer::HolderMove)], 1u);
}

GC_TEST(DeferredRemapDomain, MaxMinorAgeTracksRequeueDistance)
{
    GcHeapFixture fixture;
    MakeRetainable(fixture.region1);
    DRD::ResetForTesting(8);
    DRD::Record record = MakeRecord(
        fixture, reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE, fixture.obj1);
    record.birthMinor = 1;
    GC_EXPECT_TRUE(DRD::InsertForTesting(record));
    DRD::FlipForYoung(6);
    (void)DRD::ConsumePrevious([](const DRD::Record&) {
        return DRD::ConsumeResult::Requeue;
    });
    GC_EXPECT_EQ(DRD::GetSnapshot().maxMinorAge, 5u);
}
