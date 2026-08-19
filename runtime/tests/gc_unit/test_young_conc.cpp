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
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
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
    void Record(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
    {
        RecordCrossGenEdge(obj, fieldAddress, ref);
    }

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(ref)));
    }
};

class TestTraceBarrier final : public Barrier {
public:
    TestTraceBarrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet, BarrierPhase::TRACE) {}
    void Record(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
    {
        RecordCrossGenEdge(obj, fieldAddress, ref);
    }

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

// Stale major (Old) mark must not skip SATB during young concurrent mark.
// SatbBuffer::ShouldEnqueue used Old unconditionally; a still-young object with a
// leftover Old bit was treated as already-enqueued and never keep-alive marked.
GC_TEST(YoungConc, StaleOldMarkDoesNotSkipYoungEnqueue)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    (void)fx.PlantMarkBitmap<Generation::Old>(live, fx.region0->GetRegionSize());
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Old> oldView = fx.region0->GetMarkView<Generation::Old>();
    (void)fx.region0->GetMarkBitmap(oldView)->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(RegionSpace::ShouldEnqueue<Generation::Old>(fx.obj0));
    GC_EXPECT_TRUE(RegionSpace::ShouldEnqueue<Generation::Young>(fx.obj0));
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

// mark_and_remember mark half (zBarrier.inline.hpp:735-739): TRACE + young GC
// paints the new young target. STW/Idle TestBarrier must not (phase gate).
// gc_unit never Heap::Init; IsGcStarted is the same fixture latch SATB uses.
GC_TEST(YoungConc, TraceStoreMarksNewYoungTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestTraceBarrier barrier(collector, rs);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));
    // RecordCrossGenEdge is the remember half; TRACE phase adds the mark half.
    // Do not WriteReference: DispatchPhase would static_cast to product TraceBarrier
    // and SATB-push with a null mutator (gc_unit has no Mutator).
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;

    MarkView<Generation::Young> view = fx.region1->GetMarkView<Generation::Young>();
    GC_EXPECT_TRUE(fx.region1->IsMarkedObject(view, fx.obj1));
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 1u);

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, IdleStoreDoesNotMarkNewYoungTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_YOUNG;

    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));
    barrier.Record(fx.obj0, reinterpret_cast<MAddress>(field), fx.obj1);

    resources.SetGcStarted(startedBefore);
    resources.GetGCStats().reason = reasonBefore;

    MarkView<Generation::Young> view = fx.region1->GetMarkView<Generation::Young>();
    GC_EXPECT_FALSE(fx.region1->IsMarkedObject(view, fx.obj1));

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Peek must not consume the grey-list MergeYoungAllocBlack still owns (Stw2CurrentAudit).
GC_TEST(YoungConc, PeekYoungAllocBlackDoesNotConsume)
{
    GcHeapFixture fx;
    auto* buf = new AllocBuffer();
    buf->PushYoungAllocBlack(fx.obj0);
    std::vector<BaseObject*> peeked;
    buf->PeekYoungAllocBlack(peeked);
    GC_EXPECT_EQ(peeked.size(), 1u);
    std::vector<BaseObject*> merged;
    buf->MergeYoungAllocBlack(merged);
    GC_EXPECT_EQ(merged.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(merged[0]), reinterpret_cast<uintptr_t>(fx.obj0));
}

// Positive control: ArmInject forces uncovered+=1 so a zero cannot mean dead probe.
GC_TEST(YoungConc, Stw2CurrentInjectForcesUncovered)
{
    const size_t before = Stw2CurrentAudit::Uncovered();
    Stw2CurrentAudit::ArmInject();
    std::unordered_set<MAddress> empty;
    Stw2CurrentAudit::Census(empty, nullptr);
    GC_EXPECT_EQ(Stw2CurrentAudit::Uncovered(), before + 1);
}

GC_TEST(YoungConc, ClassifyWaterBeatsMark)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = reinterpret_cast<uintptr_t>(fx.obj0);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Water));
}

GC_TEST(YoungConc, ClassifyMarkedWhenNoWater)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region0->GetRegionSize());
    size_t off = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    MarkView<Generation::Young> view = fx.region0->GetMarkView<Generation::Young>();
    bool first = fx.region0->GetMarkBitmap(view)->MarkBits(off, 8, fx.region0->GetRegionSize());
    GC_EXPECT_FALSE(first);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Marked));
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(YoungConc, ClassifyAllocBlackWhenUnmarked)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> allocBlack;
    allocBlack.insert(fx.obj0);
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, allocBlack, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::AllocBlack));
}

GC_TEST(YoungConc, ClassifyUncoveredYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(1);
    fx.region0->SetYoungAge(1);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Uncovered));
}

GC_TEST(YoungConc, ClassifySkipOldHolderTarget)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region0->metadata.markStartAllocPtr = 0;
    std::unordered_set<BaseObject*> none;
    GC_EXPECT_EQ(static_cast<unsigned>(Stw2CurrentAudit::ClassifyTarget(fx.obj0, none, none)),
                 static_cast<unsigned>(Stw2CurrentAudit::Stw2Cover::Skip));
}
