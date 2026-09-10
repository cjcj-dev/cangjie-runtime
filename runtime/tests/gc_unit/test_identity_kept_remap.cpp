// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void RunCollectionDispatch(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_YOUNG);
        collector.DoGarbageCollection();
    }
};
} // namespace MapleRuntime

namespace {
LiveInfo* PrepareKeptGhost(GcHeapFixture& fx, RegionInfo* region, BaseObject* obj)
{
    if (region->GetRegionLifeId() == 0) {
        region->BumpRegionLifeId();
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(obj)), obj->GetSize(),
                           region->GetRegionSize());
    region->AddLiveByteCount(obj->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->SetInGhostRegion(1);
    region->metadata._generation_id = ZGenerationId::young;
    region->SetRouteState(RegionInfo::RouteState::ROUTED);
    return live;
}
} // namespace

GC_TEST(IdentityKeptRemap, ExemptProducerWaitPathReturnsIdentityWhenNotCompacted)
{
    GcHeapFixture fx;
    LiveInfo* live = PrepareKeptGhost(fx, fx.region0, fx.obj0);
    GC_EXPECT_FALSE(fx.region0->IsCompacted());

    RegionManager manager;
    manager.ExemptFromRegion(fx.region0);
    GC_EXPECT_TRUE(fx.region0->IsForwardingDone());
    GC_EXPECT_FALSE(fx.region0->IsCompacted());

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    const ForwardingTable::LookupResult produced = ForwardingTable::LookupTo(from);
    GC_EXPECT_TRUE(produced.to == from);
    GC_EXPECT_TRUE(produced.answer == ForwardingTable::ToAnswer::ArmedHit);
    std::fprintf(stderr, "IDENTITY_KEPT_PRODUCER to=%p answer=%u\n",
                 reinterpret_cast<void*>(produced.to), static_cast<unsigned>(produced.answer));

    // Retire the active carrier so the wait-path consumer (not the early armed
    // return) must accept the identity ArmedHit.
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(ForwardingTable::EntriesArmed(from));
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_TRUE(lookup.to == from);
    std::fprintf(stderr, "IDENTITY_KEPT_LOOKUP answer=%u to=%p retired=%u\n",
                 static_cast<unsigned>(lookup.answer), reinterpret_cast<void*>(lookup.to),
                 static_cast<unsigned>(lookup.retiredAnswer));

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    BaseObject* resolved = collector.relocate_or_remap_object(fx.obj0, ZGenerationId::young);
    GC_EXPECT_TRUE(resolved == fx.obj0);
    std::fprintf(stderr, "IDENTITY_KEPT_REMAP_OK from=%p resolved=%p route=%u fwdDone=%u compacted=%u\n",
                 static_cast<void*>(fx.obj0), static_cast<void*>(resolved),
                 static_cast<unsigned>(fx.region0->GetRouteState()),
                 static_cast<unsigned>(fx.region0->IsForwardingDone()),
                 static_cast<unsigned>(fx.region0->IsCompacted()));
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
    fx.region0->metadata.liveInfo = nullptr;
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
