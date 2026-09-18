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
        region0->SetRegionListOwner(nullptr);
        region1->SetRegionListOwner(nullptr);
    }

    void PrepareOldSource()
    {
        region0->reset(PageAge::old);
        region1->reset(PageAge::old);
        (void)RegionSpace::MarkObject<Generation::Old>(obj0);
        Heap::GetHeap().GetZGeneration(Generation::Old)
            .PublishPhase(ZGenerationPhase::MarkComplete);
        // zRelocationSet.cpp:110-118: select pages and install the arena before
        // preparing a source page or verifying its forwarding entries.
        region0->SetRegionListOwner(nullptr);
        RegionList selected("verify-source");
        selected.PrependRegion(region0);
        CHECK(BeginForwardingArena(Generation::Old, selected));
        (void)selected.TakeHeadRegion();
    }
};
} // namespace MapleRuntime::GcUnit
#endif
