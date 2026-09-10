// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"

#include <cstdio>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }

    static BaseObject* ResolveStoreValue(WCollector& collector, BaseObject* value)
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, value, &value };
        return collector.ResolveStoreValue(value, provenance);
    }

    static void RunYoungDispatch(WCollector& collector)
    {
        collector.SetGCReason(GC_REASON_YOUNG);
        collector.DoGarbageCollection();
    }

    static uint64_t MinorRuns(const WCollector& collector) { return collector.minorTotalRuns; }
};

} // namespace MapleRuntime

static LiveInfo* ArmGhostFrom(GcHeapFixture& fx, RegionInfo* region, BaseObject* obj)
{
    if (region->GetRegionLifeId() == 0) {
        region->BumpRegionLifeId();
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    (void)bitmap->MarkBits(offset, obj->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(obj->GetSize());
    region->metadata._generation_id = ZGenerationId::young;
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Young>());
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
    LiveInfo* live = ArmGhostFrom(fx, region, from);

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

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, from);
    GC_EXPECT_TRUE(resolved == from);
    std::fprintf(stderr,
                 "IDENTITY_KEPT_REMAP_OK from=%p resolved=%p route=%u fwdDone=%u lookup_to=%p\n",
                 static_cast<void*>(from), static_cast<void*>(resolved),
                 static_cast<unsigned>(region->GetRouteState()),
                 static_cast<unsigned>(region->IsForwardingDone()),
                 reinterpret_cast<void*>(lookup.to));

    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    if (region->IsGhostFromRegion()) {
        region->DispelGhostFromRegion();
    }
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(IdentityKeptRemap, YoungPhaseEntryDispatchesDoYoung)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);
    const uint64_t runsBefore = RelocationReceiptTestAccess::MinorRuns(collector);
    RelocationReceiptTestAccess::RunYoungDispatch(collector);
    GC_EXPECT_EQ(RelocationReceiptTestAccess::MinorRuns(collector), runsBefore + 1);
    std::fprintf(stderr, "IDENTITY_KEPT_PHASE_ENTRY_OK minorTotalRuns=%llu\n",
                 static_cast<unsigned long long>(RelocationReceiptTestAccess::MinorRuns(collector)));
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
}
