// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>
#include <dlfcn.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void BindCollector(CollectorResources& resources, TracingCollector* collector)
    {
        resources.collectorProxy.currentCollector = collector;
    }
};
} // namespace MapleRuntime

GC_TEST(IdentityKeptRemap, ExemptProducerWaitPathReturnsIdentityWhenNotCompacted)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* obj = fx.obj0;
    if (region->GetRegionLifeId() == 0) {
        region->BumpRegionLifeId();
    }
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* live = fx.PlantLiveInfo(region);
    RegionBitmap* bitmap = fx.PlantMarkBitmap<Generation::Old>(live, region->GetRegionSize());
    const MAddress from = reinterpret_cast<MAddress>(obj);
    const size_t offset = region->GetAddressOffset(from);
    (void)bitmap->MarkBits(offset, obj->GetSize(), region->GetRegionSize());
    region->AddLiveByteCount(obj->GetSize());
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
    region->RecordRouteStart(offset);
    region->metadata._generation_id = ZGenerationId::young;
    GC_EXPECT_TRUE(region->IsGhostFromRegion());
    GC_EXPECT_FALSE(region->IsCompacted());

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    RelocationReceiptTestAccess::BindCollector(resources, &collector);

    RegionManager manager;
    manager.ExemptFromRegion(region);
    GC_EXPECT_TRUE(region->IsForwardingDone());
    GC_EXPECT_FALSE(region->IsCompacted());
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    const ForwardingTable::LookupResult produced = ForwardingTable::LookupTo(from);
    std::fprintf(stderr, "IDENTITY_KEPT_PRODUCER to=%p answer=%u\n",
                 reinterpret_cast<void*>(produced.to), static_cast<unsigned>(produced.answer));
    GC_EXPECT_TRUE(produced.to == from);
    GC_EXPECT_TRUE(produced.answer == ForwardingTable::ToAnswer::ArmedHit);

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    GC_EXPECT_FALSE(ForwardingTable::EntriesArmed(from));
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    std::fprintf(stderr, "IDENTITY_KEPT_LOOKUP answer=%u to=%p retired=%u\n",
                 static_cast<unsigned>(lookup.answer), reinterpret_cast<void*>(lookup.to),
                 static_cast<unsigned>(lookup.retiredAnswer));
    GC_EXPECT_TRUE(lookup.to == from);

    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
    void* runtimeSo = dlopen("libcangjie-runtime.so", RTLD_NOLOAD | RTLD_NOW);
    GC_EXPECT_TRUE(runtimeSo != nullptr);
    auto enterScope = reinterpret_cast<void (*)()>(
        dlsym(runtimeSo, "_ZN12MapleRuntime15MutatorRelocate10EnterScopeEv"));
    auto leaveScope = reinterpret_cast<void (*)()>(
        dlsym(runtimeSo, "_ZN12MapleRuntime15MutatorRelocate10LeaveScopeEv"));
    GC_EXPECT_TRUE(enterScope != nullptr && leaveScope != nullptr);
    auto inScope = reinterpret_cast<bool (*)()>(
        dlsym(runtimeSo, "_ZN12MapleRuntime15MutatorRelocate7InScopeEv"));
    enterScope();
    std::fprintf(stderr, "IDENTITY_KEPT_INSCOPE so=%d elf=%d enter=%p\n",
                 inScope != nullptr ? static_cast<int>(inScope()) : -1,
                 static_cast<int>(MutatorRelocate::InScope()), enterScope);
    BaseObject* resolved = collector.relocate_or_remap_object(obj, ZGenerationId::young);
    leaveScope();
    GC_EXPECT_TRUE(resolved == obj);
    std::fprintf(stderr, "IDENTITY_KEPT_REMAP_OK from=%p resolved=%p route=%u fwdDone=%u compacted=%u\n",
                 static_cast<void*>(obj), static_cast<void*>(resolved),
                 static_cast<unsigned>(region->GetRouteState()),
                 static_cast<unsigned>(region->IsForwardingDone()),
                 static_cast<unsigned>(region->IsCompacted()));
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_IDLE);
    RelocationReceiptTestAccess::BindCollector(resources, nullptr);
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}


