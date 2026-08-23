// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Receipt consume/keep bound by to-region snapshotEpoch (zPage.inline.hpp:176-185)
// and by one subsequent ExpireKept (zRelocate.cpp:1018-1047).
// Does not rebind ForwardingTable::Initialize (one-shot process map).

#include "Heap/Collector/ZForwarding.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ReceiptLife, StaleToRegionEpochRejectsReceipt)
{
    GcHeapFixture fx;
    const MAddress fromStart = fx.region0->GetRegionStart();
    ZForwarding* tab = ZForwarding::Create(4, fromStart, fx.heapStart, fx.region0->GetRegionSize());
    GC_EXPECT_TRUE(tab != nullptr);

    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    tab->note_to_life(to);
    GC_EXPECT_TRUE(tab->receipt_live(to));

    const uint8_t seqBefore = fx.region1->GetRegionLifeSeq();
    fx.region1->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    fx.obj1 = fx.PlaceObject(to);
    fx.region1->SetRegionAllocPtr(to + 64);
    GC_EXPECT_TRUE(fx.region1->GetRegionLifeSeq() != seqBefore);
    GC_EXPECT_FALSE(tab->receipt_live(to));
    tab->Destroy();
}

GC_TEST(ReceiptLife, DestLaterForwardedHeaderRejectsReceipt)
{
    GcHeapFixture fx;
    const MAddress fromStart = fx.region0->GetRegionStart();
    ZForwarding* tab = ZForwarding::Create(4, fromStart, fx.heapStart, fx.region0->GetRegionSize());
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress to = reinterpret_cast<MAddress>(fx.obj1);
    tab->note_to_life(to);
    GC_EXPECT_TRUE(tab->receipt_live(to));
    fx.obj1->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_FALSE(tab->receipt_live(to));
    fx.obj1->SetStateCode(ObjectState::NORMAL);
    GC_EXPECT_TRUE(tab->receipt_live(to));
    fx.region1->SetRouteState(RegionInfo::RouteState::COMPACTED);
    GC_EXPECT_TRUE(tab->receipt_live(to));
    tab->Destroy();
}

GC_TEST(ReceiptLife, KeptExpireLatchIsOnce)
{
    ZForwarding* tab = ZForwarding::Create(4, 0x10000, 0, 0x1000);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_FALSE(tab->kept_seen_expire());
    tab->note_kept_expire();
    GC_EXPECT_TRUE(tab->kept_seen_expire());
    tab->note_kept_expire();
    GC_EXPECT_TRUE(tab->kept_seen_expire());
    tab->Destroy();
}
