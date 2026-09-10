// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"

#include <cstdio>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(IdentityKeptRemap, ArmedIdentityHitReturnsFromWhenRouteNotCompacted)
{
    GcHeapFixture fx;
    if (fx.region0->GetRegionLifeId() == 0) {
        fx.region0->BumpRegionLifeId();
    }
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetInGhostRegion(1);
    fx.region0->metadata._generation_id = ZGenerationId::young;
    fx.region0->SetRouteState(RegionInfo::RouteState::ROUTED);
    GC_EXPECT_FALSE(fx.region0->IsCompacted());

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(fx.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, from);
    publication = ForwardingTable::Publication();
    GC_EXPECT_EQ(receipt.address, from);
    fx.region0->MarkForwardingDone();

    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(lookup.to, from);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    BaseObject* resolved = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    GC_EXPECT_TRUE(resolved == fx.obj0);
    std::fprintf(stderr, "IDENTITY_KEPT_REMAP_OK from=%p resolved=%p route=%u fwdDone=%u\n",
                 static_cast<void*>(fx.obj0), static_cast<void*>(resolved),
                 static_cast<unsigned>(fx.region0->GetRouteState()),
                 static_cast<unsigned>(fx.region0->IsForwardingDone()));
}
