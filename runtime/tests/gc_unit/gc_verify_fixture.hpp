// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_GC_VERIFY_FIXTURE_HPP
#define MRT_GC_VERIFY_FIXTURE_HPP

#include "gc_heap_fixture.hpp"

namespace MapleRuntime::GcUnit {
// zVerify.cpp:125 / zHeap.inline.hpp:117: verified objects must belong to
// the heap. Keep this setup local to the verifier tests; the shared fixture
// deliberately does not initialize the complete runtime heap.
struct GcVerifyFixture : GcHeapFixture {
    GcVerifyFixture()
    {
        // Relocation preparation walks allocated objects from the page start.
        // Use a dense one-object page, without the shared fixture's empty prefix.
        obj0 = PlaceObject(region0->GetRegionStart());
        obj1 = PlaceObject(region1->GetRegionStart());
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + RegionSpace::GetAllocSize(*obj0));
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + RegionSpace::GetAllocSize(*obj1));
        region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }

    void PrepareOldSource()
    {
        region0->SetYoungRegionFlag(0);
        region1->SetYoungRegionFlag(0);
        LiveInfo* live = PlantLiveInfo(region0);
        (void)PlantMarkBitmap<Generation::Old>(live, region0->GetRegionSize());
        (void)RegionSpace::MarkObject<Generation::Old>(obj0);
        LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), Generation::Old)
            .PublishPhase(GC_PHASE_MARK_COMPLETE);
        // zRelocationSet.cpp:110-118: select pages and install the arena before
        // preparing a source page or verifying its forwarding entries.
        region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        RegionList selected("verify-source");
        selected.PrependRegion(region0, region0->GetRegionType());
        CHECK(ForwardingTable::BeginForwardingArena(Generation::Old, selected));
        (void)selected.TakeHeadRegion();
        region0->PrepareForwardableRegion(region0->GetMarkView<Generation::Old>());
    }
};
} // namespace MapleRuntime::GcUnit
#endif
