// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Region incarnation clock regression gates. The off arm proves the legacy
// decision is unchanged while audit classifies stale carriers; the enforce arm
// proves the same carriers fail closed.

#include "Heap/Collector/ZForwarding.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(LifeClock, WideClockOutlivesLegacyWrapAndHasNo32kCollision)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    const RegionLifeId first = region->GetRegionLifeId();
    const uint8_t legacyFirst = region->GetRegionLifeSeq();
    RegionLifeId previous = first;

    for (size_t i = 0; i < 128; ++i) {
        RegionInfo::InitFreeRegion(0, 1);
        region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        const RegionLifeId now = region->GetRegionLifeId();
        GC_EXPECT_EQ(now, previous + 2);
        previous = now;
    }
    GC_EXPECT_EQ(region->GetRegionLifeSeq(), legacyFirst);
    GC_EXPECT_NE(region->GetRegionLifeId(), first);

    for (size_t i = 128; i < 32768; ++i) {
        RegionInfo::InitFreeRegion(0, 1);
        region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        const RegionLifeId now = region->GetRegionLifeId();
        GC_EXPECT_EQ(now, previous + 2);
        previous = now;
    }
    GC_EXPECT_EQ(previous, first + 65536);
}

GC_TEST(LifeClock, LegacyHoldBitSurvivesIndependentClockBump)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    const RegionLifeId before = region->GetRegionLifeId();
    region->SetRouteDestHold(1);
    region->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_EQ(region->GetRegionLifeId(), before + 1);
    GC_EXPECT_TRUE(region->IsRouteDestHeld());
    region->SetRouteDestHold(0);
}

GC_TEST(LifeClock, ForgedOldMarkViewIsCountedAndEnforced)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    const RegionLifeId oldLife = region->GetRegionLifeId();

    region->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap(live, region->GetRegionSize());
    MarkView<Generation::Old> forged(region, region->GetMarkSnapshotEpoch<Generation::Old>(), oldLife);

    const auto before = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::MARK_SNAPSHOT);
    RegionBitmap* observed = region->GetMarkBitmap(forged);
    if (RegionLifeClock::EnforceEnabled()) {
        GC_EXPECT_TRUE(observed == nullptr);
    } else {
        GC_EXPECT_TRUE(observed == bitmap);
    }
    if (RegionLifeClock::Active()) {
        const auto after = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::MARK_SNAPSHOT);
        GC_EXPECT_TRUE(after.stale > before.stale);
    }

    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(LifeClock, ForgedOldRouteInfoIsCountedAndEnforced)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    constexpr uintptr_t kOldPlan = 0x22000000u;
    region->SetRouteInfo(kOldPlan, 4096);
    RouteInfo stale = region->metadata.routeInfo;

    region->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    region->metadata.routeInfo = stale;

    const auto before = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::ROUTE_INFO);
    const MAddress observed = region->GetRoutePlanAddr(0);
    GC_EXPECT_EQ(observed, RegionLifeClock::EnforceEnabled() ? static_cast<MAddress>(0)
                                                            : static_cast<MAddress>(kOldPlan));
    if (RegionLifeClock::Active()) {
        const auto after = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::ROUTE_INFO);
        GC_EXPECT_TRUE(after.stale > before.stale);
    }
}

GC_TEST(LifeClock, ReceiptAbaAfter128ReusesIsCountedAndEnforced)
{
    GcHeapFixture fx;
    const MAddress fromStart = fx.region0->GetRegionStart();
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    ZForwarding* tab = ZForwarding::Create(4, fromStart, fx.heapStart, fx.region0->GetRegionSize());
    GC_EXPECT_TRUE(tab != nullptr);
    tab->note_to_life(to);
    const uint8_t legacy = fx.region1->GetRegionLifeSeq();

    for (size_t i = 0; i < 128; ++i) {
        RegionInfo::InitFreeRegion(1, 1);
        fx.region1 = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        fx.region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    fx.obj1 = fx.PlaceObject(to);
    fx.region1->SetRegionAllocPtr(to + 64);
    GC_EXPECT_EQ(fx.region1->GetRegionLifeSeq(), legacy);

    const auto before = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::RECEIPT);
    const bool live = tab->receipt_live(to);
    GC_EXPECT_EQ(live, !RegionLifeClock::EnforceEnabled());
    if (RegionLifeClock::Active()) {
        const auto after = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::RECEIPT);
        GC_EXPECT_TRUE(after.stale > before.stale);
    }
    tab->Destroy();
}

GC_TEST(LifeClock, ArmedAndRetiredEntriesRejectOldFromPage)
{
    GcHeapFixture fx;
    RegionInfo* page = fx.region0;
    ZForwarding* tab = ZForwarding::alloc(4, page->GetRegionStart(), fx.heapStart,
                                          page->GetRegionSize(), page, page->GetRegionLifeId());
    GC_EXPECT_TRUE(tab != nullptr);

    page->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    page->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    const bool expected = !RegionLifeClock::EnforceEnabled();
    GC_EXPECT_EQ(tab->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY), expected);
    GC_EXPECT_EQ(tab->page_life_current(RegionLifeClock::Carrier::RETIRED_ENTRY), expected);
    tab->Destroy();
}

GC_TEST(LifeClock, MissingRouteStampSurvivingBoundaryIsCounted)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->metadata.routeInfo.SetRouteInfo(0x33000000u, 4096); // deliberately untracked
    const auto before = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::ROUTE_INFO);
    region->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    if (RegionLifeClock::Active()) {
        const auto after = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::ROUTE_INFO);
        GC_EXPECT_TRUE(after.zeroAcrossBoundary > before.zeroAcrossBoundary);
    }

    // The transition receipt keeps its legacy three-destination bound. Exercise
    // the fourth distinct region through the real producer so the audit matrix
    // cannot silently lose its cap_would_overflow column.
    ZForwarding* receipt = ZForwarding::Create(4, region->GetRegionStart(), fx.heapStart,
                                                region->GetRegionSize());
    GC_EXPECT_TRUE(receipt != nullptr);
    receipt->note_to_life(reinterpret_cast<MAddress>(fx.obj1));
    auto* fx2 = new GcHeapFixture();
    receipt->note_to_life(reinterpret_cast<MAddress>(fx2->obj1));
    auto* fx3 = new GcHeapFixture();
    receipt->note_to_life(reinterpret_cast<MAddress>(fx3->obj1));
    auto* fx4 = new GcHeapFixture();
    const auto capBefore = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::RECEIPT);
    receipt->note_to_life(reinterpret_cast<MAddress>(fx4->obj1));
    if (RegionLifeClock::Active()) {
        const auto capAfter = RegionLifeClock::GetSnapshot(RegionLifeClock::Carrier::RECEIPT);
        GC_EXPECT_TRUE(capAfter.capWouldOverflow > capBefore.capWouldOverflow);
    }
    receipt->Destroy();
    delete fx4;
    delete fx3;
    delete fx2;
}
