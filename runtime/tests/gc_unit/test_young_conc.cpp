// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Young concurrent-mark window invariants (REPORT-youngconc 6/20).
// Product YOUNG_CONC_MARK stays pinned-off (RegionSpace.cpp:307-310).
// These tests reconstruct the three mutator actions inside the TRACE window:
//   1. TraceBarrier-shaped SATB pre-image (ShouldEnqueue skip after paint)
//   2. TRACE-window AllocBlack (paint + grey ledger)
//   3. young→young overwrite (not remset; dirty-holder compensation)
// Shape: ZGC gtest construct-state → assert (test_zLiveMap / test_zBitMap).
//
// Compile-time arm (not MRT_GCV2_* env):
//   GC_UNIT_YOUNG_CONC_MARK_ON=0 default — models product pin
//   =1 — models allocate-black paint+ledger as if the constexpr were true

#ifndef GC_UNIT_YOUNG_CONC_MARK_ON
#define GC_UNIT_YOUNG_CONC_MARK_ON 0
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/LiveInfo.h"
#undef private

#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/Collector.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr bool kYoungConcMarkOn = (GC_UNIT_YOUNG_CONC_MARK_ON != 0);

class TestCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
    }
};

class TestBarrier final : public Barrier {
public:
    TestBarrier(Collector& collector, RememberedSet& rememberedSet) : Barrier(collector, rememberedSet) {}

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
    }
};

// Product AllocBlack (RegionSpace.cpp:326-346) when youngConcMarkOn.
void ProductAllocBlackPaint(RegionInfo* reg, BaseObject* obj, size_t totalSize, AllocBuffer* ledger)
{
    if (!kYoungConcMarkOn || reg == nullptr || reg->IsLargeRegion() || !reg->IsYoungRegion()) {
        return;
    }
    MAddress addr = reinterpret_cast<MAddress>(obj);
    MAddress regionStart = reg->GetRegionStart();
    MAddress regionEnd = reg->GetRegionEnd();
    size_t offset = static_cast<size_t>(addr - regionStart);
    size_t regionSize = static_cast<size_t>(regionEnd - regionStart);
    if (totalSize == 0 || (totalSize % 8) != 0 || offset + totalSize > regionSize) {
        return;
    }
    MarkView<Generation::Young> view = reg->GetMarkView<Generation::Young>();
    RegionBitmap* bm = reg->GetMarkBitmap(view);
    if (bm == nullptr) {
        return;
    }
    bool already = bm->MarkBits(offset, totalSize, regionSize);
    if (!already) {
        reg->AddLiveByteCount(totalSize);
    }
    LiveInfo* ghost = reg->GetLiveInfo0ForProbe();
    RegionBitmap* ghostBitmap = ghost == nullptr ? nullptr : reg->GetRouteMarkBitmap(ghost);
    if (ghost != nullptr && ghostBitmap != nullptr) {
        (void)ghostBitmap->MarkBits(offset, totalSize, regionSize);
    }
    if (ledger != nullptr) {
        ledger->PushYoungAllocBlack(obj);
    }
}

} // namespace

// 1. SATB / TraceBarrier write: after AllocBlack paint, ShouldEnqueue is false
//    (ZGC zBarrier.inline.hpp:735-740 mark_and_remember; our SATB skips marked).
GC_TEST(YoungConc, PaintedObjectSkippedByShouldEnqueue)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(fx.region0->GetMarkBitmap(view) != nullptr);
    (void)fx.region0->GetMarkBitmap(view)->MarkBits(off, 8, fx.region0->GetRegionSize());

    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// isTraceRegion alone skips SATB (ShouldEnqueue) — paint required or object stays white.
GC_TEST(YoungConc, TraceRegionSkipsSatbWithoutPaint)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->SetTraceRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// 2. AllocBlack: pin-off leaves white; constexpr-on paints + greys (RegionSpace.cpp:299-346).
GC_TEST(YoungConc, AllocBlackWhiteWhenConcMarkOff)
{
    if (kYoungConcMarkOn) {
        return;
    }
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    auto* buf = new AllocBuffer();
    ProductAllocBlackPaint(fx.region0, fx.obj0, 8, buf);

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_FALSE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));
    std::vector<BaseObject*> stack;
    buf->MergeYoungAllocBlack(stack);
    GC_EXPECT_EQ(stack.size(), 0u);

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, AllocBlackPaintAndGreyWhenConcMarkOn)
{
    if (!kYoungConcMarkOn) {
        return;
    }
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    auto* buf = new AllocBuffer();
    ProductAllocBlackPaint(fx.region0, fx.obj0, 8, buf);

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), off));
    std::vector<BaseObject*> stack;
    buf->MergeYoungAllocBlack(stack);
    GC_EXPECT_EQ(stack.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stack[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Paint-then-claim-skip: already-marked MarkObject returns true; without grey ledger
// TraceYoungClosure would drop reachableVec/fields (WCollector.cpp:8110-8136).
GC_TEST(YoungConc, PaintWithoutGreyLedgerMissesWorkStack)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());

    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    RegionBitmap* bm = fx.region0->GetMarkBitmap(view);
    GC_EXPECT_TRUE(bm != nullptr);
    bool first = bm->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(first);
    bool claim = bm->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_TRUE(claim);

    auto* empty = new AllocBuffer();
    std::vector<BaseObject*> missed;
    empty->MergeYoungAllocBlack(missed);
    GC_EXPECT_EQ(missed.size(), 0u);

    auto* grey = new AllocBuffer();
    grey->PushYoungAllocBlack(fx.obj0);
    std::vector<BaseObject*> found;
    grey->MergeYoungAllocBlack(found);
    GC_EXPECT_EQ(found.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(found[0]), reinterpret_cast<uintptr_t>(fx.obj0));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// 3. young→young overwrite is not remset (ZGC remember only if slot old; zBarrier:729-733).
GC_TEST(YoungConc, YoungToYoungWriteNotInRemset)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// Compensation: y2y dirty holder is merged into work stack (AllocBuffer.h:84-105).
GC_TEST(YoungConc, YoungToYoungDirtyHolderReachesWorkStack)
{
    GcHeapFixture fx;
    auto* buf = new AllocBuffer();
    buf->PushY2yDirtyHolder(fx.obj0);
    buf->PushY2yDirtyHolder(fx.obj0);
    GC_EXPECT_EQ(buf->Y2yDirtyHolderCount(), 1u);
    std::vector<BaseObject*> stack;
    buf->MergeY2yDirtyHolders(stack);
    GC_EXPECT_EQ(stack.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stack[0]), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_EQ(buf->Y2yDirtyHolderCount(), 0u);
}

// old→young still remset (control: TRACE window must not drop the only remset edge).
GC_TEST(YoungConc, OldToYoungStillRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 1u);
    GC_EXPECT_TRUE(records.count(reinterpret_cast<MAddress>(field)) == 1);
}
