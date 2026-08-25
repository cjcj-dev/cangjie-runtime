// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U7 — cross-gen remset: old→young write must enter RememberedSet via product Barrier path.
// Product symbols: Barrier::WriteReference → Barrier::RecordCrossGenEdge, RememberedSet::Record.
// Defect anchor: idleedge (Idle window missed bare old→young edges).
// Acceptance gate for wbclose2: after Idle barrier records, this stays green;
// if Idle WriteReferenceImpl skips MCC / RecordCrossGenEdge, Idle phase arm must go red.

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_set>

// RememberedSet::Record is private with `friend class Barrier` -- friendship a derived TestBarrier
// does not inherit.  The minor's own consumer (WCollector::RescanRememberedSet) calls Record
// directly when it re-arms a scanned slot, so a test that cannot call it cannot model the re-arm at
// all.  Same idiom the fixture already uses for RegionInfo; scoped to this one header.
#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private

#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_PostWriteRefField(ObjectPtr ref, ObjectPtr obj, RefField<false>* field);

namespace {

class InstalledBarrierScope {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }

    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
};

class TestCollector final : public Collector {
public:
    explicit TestCollector(bool colourStores = false) : colourStores(colourStores) {}

    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        if (colourStores) {
            return RefField<>(obj, ::g_cjStoreGoodMask);
        }
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(obj)));
    }

private:
    bool colourStores;
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

bool ExpectRecorded(RememberedSet& rs, MAddress fieldAddr)
{
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    return std::find(records.begin(), records.end(), fieldAddr) != records.end() && records.size() == 1;
}

class ScopedEnv final {
public:
    ScopedEnv(const char* name, const char* value) : name(name)
    {
        const char* oldValue = std::getenv(name);
        if (oldValue != nullptr) {
            hadOldValue = true;
            savedValue = oldValue;
        }
        if (value == nullptr) {
            (void)unsetenv(name);
        } else {
            (void)setenv(name, value, 1);
        }
    }

    ~ScopedEnv()
    {
        if (hadOldValue) {
            (void)setenv(name.c_str(), savedValue.c_str(), 1);
        } else {
            (void)unsetenv(name.c_str());
        }
    }

private:
    std::string name;
    std::string savedValue;
    bool hadOldValue = false;
};

} // namespace

GC_TEST(Remset, CurrentMinorRootOverridesRetainedDeadSnapshotAndNullHeal)
{
    GC_EXPECT_TRUE(KeepRememberedHolder(true, false));
    GC_EXPECT_TRUE(KeepRememberedHolder(true, true));
    GC_EXPECT_TRUE(KeepRememberedHolder(false, true));
    GC_EXPECT_FALSE(KeepRememberedHolder(false, false));
}

// Validation-only sticky storage is absent unless explicitly armed.
GC_TEST(Remset, StickyBitmapDisabledByDefault)
{
    ScopedEnv gate("MRT_GCV2_REMSET_EVER", nullptr);
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    GC_EXPECT_FALSE(rs.EverRecordedEnabled());
}

// When armed, the sticky bit proves a mutator barrier recorded the slot and survives a drain.
GC_TEST(Remset, StickyBitmapTracksMutatorBarrierAcrossDrain)
{
    ScopedEnv gate("MRT_GCV2_REMSET_EVER", "1");
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    GC_EXPECT_TRUE(rs.EverRecordedEnabled());
    GC_EXPECT_FALSE(rs.WasEverRecorded(slot));
    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(rs.Contains(slot));
    GC_EXPECT_TRUE(rs.WasEverRecorded(slot));

    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_FALSE(rs.Contains(slot));
    GC_EXPECT_TRUE(rs.WasEverRecorded(slot));
}

// U7: product Barrier NVI WriteReference records old→young edge.
GC_TEST(Remset, OldToYoungRecordedByBarrier)
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
    GC_EXPECT_TRUE(ExpectRecorded(rs, reinterpret_cast<MAddress>(field)));
}

// Case A: a non-null plain previous word is not store-good.  The product barrier must still
// register the old slot; the cheap compiler-side negative-mask predicate cannot be used as the
// remembered-set witness for this value.
GC_TEST(Remset, PlainPreviousWordStillRecordsOldToYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);

    // Deliberately install a non-null, uncoloured previous word (case A).
    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj1)));
    GC_EXPECT_FALSE(collector.is_store_good(*field));
    barrier.WriteReference(fx.obj0, *field, fx.obj1);

    GC_EXPECT_TRUE(rs.Contains(slot));
}

// Case B: the slot was already registered, then the current remset face was drained before the
// next write.  A store-good/same-target fast-path decision must not suppress the new current-face
// registration after DrainForMinor has cleared the prior bitmap.
GC_TEST(Remset, StoreGoodRewriteReregistersAfterDrain)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> firstMinor;
    rs.DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(slot) == 1);
    GC_EXPECT_EQ(rs.Size(), 0u);
    GC_EXPECT_TRUE(collector.is_store_good(*field));

    barrier.WriteReference(fx.obj0, *field, fx.obj1);

    GC_EXPECT_TRUE(rs.Contains(slot));
}

// Compiler hit arm analogue for case A. The coloured store is already done,
// so the product post-store exit must be the operation that makes the slot
// observable in the current remembered-set face.
GC_TEST(Remset, CompilerPostStoreRecordsPlainPreviousWord)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    InstalledBarrierScope installedBarrier(barrier);

    field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fx.obj0)));
    GC_EXPECT_FALSE(collector.is_store_good(*field));
    RefField<> installed = collector.GetAndTryTagRefField(fx.obj1);
    field->StoreColoured(installed.GetFieldValue());
    GC_EXPECT_FALSE(rs.Contains(slot)); // positive control: the direct store alone does not record

    CJ_MCC_PostWriteRefField(fx.obj1, fx.obj0, field);
    GC_EXPECT_TRUE(rs.Contains(slot));
}

// Exact T0/T1 construction from case B. T0 has no young region, so the old
// target store paints the slot but records nothing. T1 installs a young target
// through the compiler-like hit arm; only the post-store product exit records it.
GC_TEST(Remset, CompilerPostStoreRecordsChangedTargetAfterNoYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(0);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    InstalledBarrierScope installedBarrier(barrier);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj0);
    GC_EXPECT_TRUE(collector.is_store_good(*field));
    GC_EXPECT_FALSE(rs.Contains(slot));

    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    RefField<> installed = collector.GetAndTryTagRefField(fx.obj1);
    field->StoreColoured(installed.GetFieldValue());
    GC_EXPECT_FALSE(rs.Contains(slot)); // pre-fix compiler hit result

    CJ_MCC_PostWriteRefField(fx.obj1, fx.obj0, field);
    GC_EXPECT_TRUE(rs.Contains(slot));
}

GC_TEST(Remset, AtomicWriteRecordsOldToYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.AtomicWriteReference(fx.obj0, *field, fx.obj1, std::memory_order_seq_cst);
    GC_EXPECT_TRUE(rs.Contains(slot));
}

GC_TEST(Remset, AtomicSwapRecordsOldToYoung)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    BaseObject* old = barrier.AtomicSwapReference(fx.obj0, *field, fx.obj1, std::memory_order_seq_cst);
    GC_EXPECT_TRUE(old == nullptr);
    GC_EXPECT_TRUE(rs.Contains(slot));
}

GC_TEST(Remset, CompareAndSwapRecordsOnlySuccessfulStore)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector(true);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    GC_EXPECT_FALSE(barrier.CompareAndSwapReference(fx.obj0, *field, fx.obj1, fx.obj1,
                                                    std::memory_order_seq_cst,
                                                    std::memory_order_seq_cst));
    GC_EXPECT_FALSE(rs.Contains(slot));

    GC_EXPECT_TRUE(barrier.CompareAndSwapReference(fx.obj0, *field, nullptr, fx.obj1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst));
    GC_EXPECT_TRUE(rs.Contains(slot));
}

// U7: product IdleBarrier WriteReference also records (wbclose2 acceptance).
// Pre-fix Idle may skip MCC; product base NVI still posts RecordCrossGenEdge after
// WriteReferenceImpl — so this is green when NVI post-record is intact.
// Red when RecordCrossGenEdge is broken or young flag missing.
GC_TEST(Remset, IdleBarrierOldToYoungRecorded)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    IdleBarrier idle(collector, rs);

    field->StoreColoured(zpointer::null);
    idle.WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(rs, reinterpret_cast<MAddress>(field)));
}

// Static roots are enumerated directly by every minor and must not enter the
// heap-only remembered set. Restoring the external record path makes this red.
GC_TEST(Remset, StaticRootNotRecorded)
{
    GcHeapFixture fx;
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    RootSlot root;

    barrier.WriteStaticRef(root, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// U7: young→young must NOT enter remset (only old→young).
GC_TEST(Remset, YoungToYoungNotRecorded)
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

// The write barrier conditions on the SLOT's generation, never the target's.  OpenJDK,
// zBarrier.inline.hpp:729-733:
//
//     inline void ZBarrier::remember(volatile zpointer* p) {
//       if (ZHeap::heap()->is_old(p)) {
//         ZGeneration::young()->remember(p);
//       }
//     }
//
// so an old->old store does enter the remembered set there too.  That is deliberate: testing the
// target would put a load and a page-table lookup on every reference store, and the entry is cheap
// to discard later.  The discard is the other half, ZRemembered::scan_field
// (zRemembered.cpp:578-589): a scanned slot is re-armed only while its healed target is still young,
// so old->old entries evaporate after one young cycle instead of being kept out up front.
//
// This test previously asserted the opposite ("old->old must NOT enter remset") and went red when
// RecordCrossGenEdge stopped filtering on the target generation (abe3c4d8) -- a change made because
// that filter was dropping real old->young edges (UNMARKED_LIVE 895->0, edgeNotInRemset 28->0).  The
// expectation was the stale half, not the fix.
GC_TEST(Remset, OldToOldRecordedBecauseBarrierConditionsOnSlot)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(0);
    // RecordCrossGenEdge has an intentional process-wide no-young fast exit.
    // Make the test's precondition explicit instead of borrowing a leaked
    // youngRegionCount from an earlier fixture.
    RegionInfo* youngWitness =
        RegionInfo::InitRegion(2, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    youngWitness->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    youngWitness->SetYoungRegionFlag(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(rs, reinterpret_cast<MAddress>(field)));
    youngWitness->SetYoungRegionFlag(0);
}

// The sticky provenance bitmap is diagnostic backing, not product state.  Its
// positive control must cover all three claims the probe makes: the env really
// allocates it, a mutator-barrier record flips the bit, and a destructive minor
// drain does not clear it.
GC_TEST(Remset, EverRecordedStickyBitmapPositiveControl)
{
    GcHeapFixture fx;
    const MAddress slot = reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE;

    (void)unsetenv("MRT_GCV2_REMSET_EVER");
    RememberedSet off;
    off.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    GC_EXPECT_FALSE(off.EverRecordedEnabled());
    GC_EXPECT_FALSE(off.WasEverRecorded(slot));

    GC_EXPECT_EQ(setenv("MRT_GCV2_REMSET_EVER", "1", 1), 0);
    RememberedSet on;
    on.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    GC_EXPECT_TRUE(on.EverRecordedEnabled());
    GC_EXPECT_FALSE(on.WasEverRecorded(slot));

    on.Record(slot, true);
    GC_EXPECT_TRUE(on.WasEverRecorded(slot));
    std::unordered_set<MAddress> drained;
    on.DrainForMinor(drained);
    GC_EXPECT_TRUE(on.WasEverRecorded(slot));
    (void)unsetenv("MRT_GCV2_REMSET_EVER");
}

// ---------------------------------------------------------------------------------------------
// Edge retention across minors.
//
// The write barrier records an old->young edge exactly once, at the store.  DrainForMinor is
// destructive: it swaps in an empty bitmap and hands the caller the old one.  So a field that is
// written once and never again is remembered for exactly one minor, while the edge it describes
// stays in the heap indefinitely.  From the second minor on, the young object is reachable only
// through a slot nobody scans.
//
// OpenJDK does not prevent this at record time -- ZBarrier::remember (zBarrier.inline.hpp:729-733)
// conditions only on the slot being old, and never looks at the target.  It repairs it at scan
// time instead: ZRemembered::scan_field (zRemembered.cpp:578-589) re-arms every scanned slot whose
// healed target is still young, and drops the rest by simply not re-arming them.
//
// We do the same in WCollector::RescanRememberedSet.  That fix rests on one property of this class
// that nothing tested: a Record() issued while consuming a drained set must land in the *next*
// cycle's buffer.  If it landed in the one being drained the re-arm would either be lost or loop.
//
// Found the slow way first: with the re-arm off, natural_wave_notime showed 468 unmarked-live young
// objects in 1 of 63 minor windows, one of which had an incoming old->young edge absent from the
// remembered set.  1-in-63 is not something to go fishing for; these two tests are that same
// statement, deterministic and in tens of milliseconds.

GC_TEST(Remset, DrainIsDestructiveSoAnEdgeWrittenOnceIsLost)
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

    std::unordered_set<MAddress> firstMinor;
    rs.DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(reinterpret_cast<MAddress>(field)) == 1);

    // No second write: the edge is still in the heap, the record is not.
    std::unordered_set<MAddress> secondMinor;
    rs.DrainForMinor(secondMinor);
    GC_EXPECT_EQ(secondMinor.size(), 0u);
}

GC_TEST(Remset, ReRecordWhileConsumingLandsInTheNextCycleBuffer)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TestBarrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);

    std::unordered_set<MAddress> firstMinor;
    rs.DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(slot) == 1);
    GC_EXPECT_EQ(rs.Size(), 0u);

    // What RescanRememberedSet does for a scanned slot whose target is still young.  Twice, because
    // a slot can be reached through more than one path in a cycle and the re-arm must be idempotent
    // rather than accumulate.
    rs.Record(slot);
    rs.Record(slot);
    GC_EXPECT_EQ(rs.Size(), 1u);

    std::unordered_set<MAddress> secondMinor;
    rs.DrainForMinor(secondMinor);
    GC_EXPECT_TRUE(secondMinor.count(slot) == 1);
    GC_EXPECT_EQ(secondMinor.size(), 1u);

    // And it self-drains: a cycle that does not re-arm gives the slot up, which is how an edge whose
    // target has been promoted out of young stops costing a scan.
    std::unordered_set<MAddress> thirdMinor;
    rs.DrainForMinor(thirdMinor);
    GC_EXPECT_EQ(thirdMinor.size(), 0u);
}
