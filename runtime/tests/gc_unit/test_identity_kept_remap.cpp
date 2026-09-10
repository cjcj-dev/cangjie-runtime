// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"

#include <cstdio>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void RunCollectionDispatch(WCollector& collector);
};
} // namespace MapleRuntime

static LiveInfo* ArmGhostForKept(GcHeapFixture& fx, RegionInfo* region, BaseObject* obj)
{
    if (region->GetRegionLifeId() == 0) {
        region->BumpRegionLifeId();
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    (void)bitmap->MarkBits(offset, obj->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(obj->GetSize());
    region->metadata._generation_id = ZGenerationId::young;
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->RecordRouteStart(offset);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_FALSE(region->IsCompacted());
    return live;
}

GC_TEST(IdentityKeptRemap, ArmedIdentityHitReturnsFromWhenRouteNotCompacted)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* from = fx.obj0;
    LiveInfo* live = ArmGhostForKept(fx, region, from);

    RegionManager manager;
    manager.ExemptFromRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_FALSE(region->IsCompacted());

    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->SetInGhostRegion(1);

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    GC_EXPECT_FALSE(ForwardingTable::EntriesArmed(reinterpret_cast<MAddress>(from)));
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit ||
                   lookup.retiredAnswer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_EQ(lookup.to, reinterpret_cast<MAddress>(from));
    std::fprintf(stderr, "IDENTITY_KEPT_LOOKUP answer=%u retired=%u to=%p identity=%d\n",
                 static_cast<unsigned>(lookup.answer),
                 static_cast<unsigned>(lookup.retiredAnswer),
                 reinterpret_cast<void*>(lookup.to),
                 lookup.to == reinterpret_cast<MAddress>(from) ? 1 : 0);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    BaseObject* resolved = collector.relocate_or_remap_object(from, ZGenerationId::young);
    GC_EXPECT_TRUE(resolved == from);
    std::fprintf(stderr,
                 "IDENTITY_KEPT_REMAP_OK from=%p resolved=%p route=%u fwdDone=%u lookup_to=%p\n",
                 static_cast<void*>(from), static_cast<void*>(resolved),
                 static_cast<unsigned>(region->GetRouteState()),
                 static_cast<unsigned>(region->IsForwardingDone()),
                 reinterpret_cast<void*>(lookup.to));

    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(IdentityKeptRemap, YoungPhaseEntryDispatch)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::RunCollectionDispatch(collector);
    std::fprintf(stderr, "IDENTITY_KEPT_PHASE_ENTRY_OK\n");
}
