// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ZGC zStoreBarrierBuffer.cpp:173-182. A store buffered before young-mark
// start is too late for the bitmap that mark already published. The phase
// flush must scan that old slot and remember it, so the next young mark still
// resolves the young referent after the remembered-set flip.
GC_TEST(OldToYoung1102, YoungMarkPhaseFlushRemembersOldToYoungSlot)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, GcHeapFixture::kUnits * ZGranuleSize);

    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    const MAddress slot = reinterpret_cast<MAddress>(&field);

    StoreBarrierBuffer buffer;
    buffer.Initialize(::g_cjStoreGoodMask);
    buffer.add(slot, field.GetFieldValue());
    buffer.lastProcessedColor = ::g_cjStoreGoodMask ^ ZPointerMarkedYoungMask;

    auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    const auto phaseBefore = old.GcPhase();
    old.set_phase(ZGenerationPhase::MarkComplete);
    buffer.on_new_phase();
    old.set_phase(ZGenerationPhase::Mark);

    const bool published = SlotPageRemembered(slot);
    ZRememberedSet::flip();
    const bool scanned = Heap::GetHeap().remembered().scan_page_and_clear_remset(fx.region0);
    ZRememberedSet::flip();
    old.set_phase(phaseBefore);

    const zaddress resolved = ZBarrier::load_barrier_on_oop_field(reinterpret_cast<volatile zpointer*>(&field));
    BaseObject* target = to_object(resolved);
    ZPage* page = Heap::page(reinterpret_cast<MAddress>(target));
    std::fprintf(stderr,
                 "OLD_TO_YOUNG_1102_RESOLVED published=%d scanned=%d target=%p young=%p page=%p\n",
                 published, scanned, static_cast<void*>(target), static_cast<void*>(fx.obj1),
                 static_cast<void*>(page));
    std::fflush(stderr);
    GC_EXPECT_TRUE(published && scanned && target == fx.obj1 && page != nullptr && page->IsYoungRegion());
}
