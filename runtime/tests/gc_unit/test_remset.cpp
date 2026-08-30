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
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif
#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private

#include "Heap/Barrier/Barrier.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"
#include "Heap/Verify/NwDropAudit.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_PostWriteRefField(ObjectPtr ref, ObjectPtr obj, RefField<false>* field,
                                          uintptr_t observedPrev);

namespace MapleRuntime {

// The product minor path drains the previous face in Generation.cpp and then
// hands that exact set to WCollector::RescanRememberedSet. Keep this test peer
// limited to that hand-off; all filtering, resolving, marking and re-arming
// remain in the product function compiled into libcangjie-runtime.so.
struct RemsetRearmTestAccess {
    struct ConsumeResult {
        size_t work = 0;
        size_t consumedLedger = 0;
        RemsetScanStats stats;
    };

    static RefField<> Tag(WCollector& collector, BaseObject* object)
    {
        return collector.GetAndTryTagRefField(object);
    }

    static void BeginMinor(WCollector& collector)
    {
        // WCollector::DoYoungGarbageCollection publishes the new young mark and
        // remembered colours before RememberedSet::DrainForMinor (Generation.cpp:533,649-651).
        collector.flip_young_mark_start();
    }

    static bool FixInteriorSlot(WCollector& collector, RefField<>& field, BaseObject* knownBase)
    {
        return collector.FixMinorEvacuatedSlot(field, knownBase, nullptr, false);
    }

    static ConsumeResult ConsumePrevious(WCollector& collector, const std::unordered_set<MAddress>& previous,
                                          BaseObject* currentMinorRoot)
    {
        WCollector::WorkStack workStack = collector.NewWorkStack();
        WCollector::MinorSlotSet reachableSlots;
        WCollector::MinorSlotSet weakSlots;
        WCollector::MinorObjectSet currentMinorRoots;
        WCollector::MinorSlotSet consumed;
        RemsetScanStats stats;
        stats.recorded = previous.size();
        if (currentMinorRoot != nullptr) {
            currentMinorRoots.insert(currentMinorRoot);
        }
        collector.RescanRememberedSet(workStack, previous, reachableSlots, weakSlots, currentMinorRoots,
                                      /*fullYoungScan=*/false, &consumed, &stats);
        const size_t work = workStack.size();
        while (!workStack.empty()) {
            workStack.pop_back();
        }
        return ConsumeResult { work, consumed.size(), stats };
    }
};

} // namespace MapleRuntime

namespace {

// Product-path guard for the three relocate interior writebacks. The slot
// starts load-good but not store-good, so deleting the product call leaves a
// legal yet stale colour and this exact assertion fails.
GC_TEST(RelocateInterior, MinorFixPublishesCurrentStoreGoodColour)
{
    GcHeapFixture fx;
    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    BaseObject* interior = reinterpret_cast<BaseObject*>(
        reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t desired = MakeStoreGoodSlotWord(
        reinterpret_cast<uintptr_t>(interior), static_cast<uintptr_t>(::g_cjStoreGoodMask));
    // Change only the remembered epoch.  The word remains load/mark-good, so
    // ResolveMinorReference returns the payload without rewriting the slot;
    // the interior StoreGood publication below is therefore the sole repair.
    const uintptr_t initial = desired ^ REMEMBERED_MASK;
    field->StoreColoured(to_zpointer(initial));

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    const bool changed = RemsetRearmTestAccess::FixInteriorSlot(collector, *field, fx.obj1);
    const uintptr_t finalWord = raw(field->GetFieldValue());
    std::fprintf(stderr,
                 "DETAIL relocate_interior initial=%#zx desired=%#zx final=%#zx changed=%u "
                 "verdict=%u\n",
                 static_cast<size_t>(initial), static_cast<size_t>(desired),
                 static_cast<size_t>(finalWord), static_cast<unsigned>(changed),
                 static_cast<unsigned>(ClassifySlotWord(finalWord)));
    GC_EXPECT_NE(initial, desired);
    GC_EXPECT_EQ(finalWord, desired);
    GC_EXPECT_TRUE(ClassifySlotWord(finalWord) == SlotWordVerdict::kColoured);
}

// Receipt-channel positive controls.  These deliberately exercise each
// test-only counter with distinct fixed slots, then reset the native buffer
// and require an exact all-zero baseline.  Product-path wiring is covered by
// StoreGoodAfterProductConsumerRearm below; this arm only proves that the
// four reason channels cannot collapse into one another.
GC_TEST(Remset, Wave8FilterReceiptPositiveControls)
{
#if defined(MRT_GC_UNIT_TESTS)
    ResetRemsetFilterTestReceipt();
    NoteRemsetFilterTestReceipt(0x1000, RemsetFilterReceiptReason::kStale, false);
    NoteRemsetFilterTestReceipt(0x2000, RemsetFilterReceiptReason::kDeadHolder, false);
    NoteRemsetFilterTestReceipt(0x3000, RemsetFilterReceiptReason::kNoOrigin, false);
    NoteRemsetFilterTestReceipt(0x4000, RemsetFilterReceiptReason::kBadTarget, false);
    const auto positive = ReadRemsetFilterTestReceipt();
    std::fprintf(stderr,
                 "DETAIL wave8_filter_controls stale=%zu dead_holder=%zu no_origin=%zu bad_target=%zu\n",
                 static_cast<size_t>(positive.stale), static_cast<size_t>(positive.deadHolder),
                 static_cast<size_t>(positive.noOrigin), static_cast<size_t>(positive.badTarget));
    GC_EXPECT_EQ(positive.stale, 1u);
    GC_EXPECT_EQ(positive.deadHolder, 1u);
    GC_EXPECT_EQ(positive.noOrigin, 1u);
    GC_EXPECT_EQ(positive.badTarget, 1u);
    ResetRemsetFilterTestReceipt();
    const auto zero = ReadRemsetFilterTestReceipt();
    std::fprintf(stderr,
                 "DETAIL wave8_filter_controls_reset stale=%zu dead_holder=%zu no_origin=%zu bad_target=%zu\n",
                 static_cast<size_t>(zero.stale), static_cast<size_t>(zero.deadHolder),
                 static_cast<size_t>(zero.noOrigin), static_cast<size_t>(zero.badTarget));
    GC_EXPECT_EQ(zero.seen, 0u);
    GC_EXPECT_EQ(zero.stale, 0u);
    GC_EXPECT_EQ(zero.deadHolder, 0u);
    GC_EXPECT_EQ(zero.noOrigin, 0u);
    GC_EXPECT_EQ(zero.badTarget, 0u);
#endif
}

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
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override
    {
        return FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(GcUnit::StoreGoodPointer(obj));
    }
};

class TestBarrier final : public Barrier {
public:
    TestBarrier(Collector& collector, RememberedSet& rememberedSet) : Barrier(collector, rememberedSet) {}

protected:
    void WriteReferenceImpl(BaseObject*, RefField<false>& field, BaseObject* ref) const
    {
        field.StoreColoured(GcUnit::StoreGoodPointer(ref));
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

// Case A: ZGC's single-negative-mask predicate classifies a non-null plain
// previous word as store-good, while the heap-slot encoding gate classifies
// the same word as illegal.  The product barrier must still register the old
// slot; encoding legality must not be inferred from the fast-path predicate.
GC_TEST(Remset, PlainPreviousWordStillRecordsOldToYoung)
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
    Barrier barrier(collector, rs);

    // Deliberately install a non-null, uncoloured previous word (case A).
    const uintptr_t plain = reinterpret_cast<uintptr_t>(fx.obj1);
    std::memcpy(field, &plain, sizeof(plain));
    GC_EXPECT_TRUE(collector.is_store_good(*field));
    GC_EXPECT_EQ(ClassifySlotWord(plain), SlotWordVerdict::kIllegal);
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
    TestCollector collector;
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

// Product-path form of the r6b failure arm. Generation drains the previous
// face, then WCollector::RescanRememberedSet consumes it and re-arms the slot
// while its resolved target is still young (zRemembered.cpp:578-589). The
// compiler-like bare store below makes no runtime call; minor #2 must still
// receive the slot from the current face established by that product consumer.
GC_OTHER_VM_TEST(Remset, StoreGoodAfterProductConsumerRearm)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    // Product remset consumption accepts exact object starts through the loaded
    // TypeInfo registry (Remembered.cpp:1157-1204). GcHeapFixture normally needs
    // residence only; this product-entry test needs the stronger real-object precondition.
    fx.typeInfo->SetUUID(1);
    TypeInfoManager::GetTypeInfoManager().AddTypeInfo(fx.typeInfo);
    const bool targetTypeRegistered = TypeInfoManager::GetTypeInfoManager().ContainsTypeInfo(fx.typeInfo);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    BaseObject* objectB = fx.PlaceObject(fx.heapStart + RegionInfo::UNIT_SIZE + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(objectB) + 64);

    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    Barrier barrier(collector, rs);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    RemsetRearmTestAccess::BeginMinor(collector);
#if defined(MRT_GC_UNIT_TESTS)
    ResetRemsetFilterTestReceipt();
#endif
    std::unordered_set<MAddress> firstMinor;
    const size_t firstDrainCount = rs.DrainForMinor(firstMinor);
    const size_t firstCount = firstMinor.count(slot);
    const size_t sizeAfterFirstDrain = rs.Size();
    const auto firstConsume = RemsetRearmTestAccess::ConsumePrevious(collector, firstMinor, fx.obj0);
#if defined(MRT_GC_UNIT_TESTS)
    const auto firstReceipt = ReadRemsetFilterTestReceipt();
    NwDropAudit::Report("wave8_receipt");
#endif
    const size_t sizeAfterFirstConsume = rs.Size();

    // LLVM store-good hit analogue: coloured volatile store, no runtime hand-off.
    RefField<> taggedB = RemsetRearmTestAccess::Tag(collector, objectB);
    field->StoreColoured(taggedB.GetFieldValue());
    const uintptr_t fieldBeforeSecondDrain = raw(field->GetFieldValue());
    const bool containsBeforeSecondDrain = rs.Contains(slot);
    RemsetRearmTestAccess::BeginMinor(collector);
    std::unordered_set<MAddress> secondMinor;
    const size_t secondDrainCount = rs.DrainForMinor(secondMinor);
    const size_t secondCount = secondMinor.count(slot);
    const size_t sizeAfterSecondDrain = rs.Size();
    const auto secondConsume = RemsetRearmTestAccess::ConsumePrevious(collector, secondMinor, fx.obj0);
    const size_t sizeAfterSecondConsume = rs.Size();

    std::fprintf(stderr,
                 "DETAIL arm=product_consumer slot=0x%zx first_drain=%zu first_count=%zu "
                 "size_after_first_drain=%zu first_consumer_work=%zu first_consumed=%zu "
                 "first_stats_consumed=%zu first_skipped_not_heap=%zu first_skipped_weak=%zu "
                 "target_type_registered=%u size_after_first_consume=%zu "
                 "field_before_second_drain=0x%zx remset_before_second_drain=%u "
                 "second_drain=%zu second_count=%zu size_after_second_drain=%zu "
                 "second_consumer_work=%zu second_consumed=%zu second_stats_consumed=%zu "
                 "second_skipped_not_heap=%zu second_skipped_weak=%zu "
                 "size_after_second_consume=%zu target_young=%u "
#if defined(MRT_GC_UNIT_TESTS)
                 "receipt_seen=%zu receipt_consumed=%zu receipt_stale=%zu receipt_dead=%zu "
                 "receipt_no_origin=%zu receipt_bad_target=%zu "
#endif
                 "invariant=slot_present_after_bare_store\n",
                 static_cast<size_t>(slot), firstDrainCount, firstCount, sizeAfterFirstDrain,
                 firstConsume.work, firstConsume.consumedLedger, firstConsume.stats.consumed,
                 firstConsume.stats.skippedNotHeap, firstConsume.stats.skippedWeak,
                 static_cast<unsigned>(targetTypeRegistered), sizeAfterFirstConsume,
                 static_cast<size_t>(fieldBeforeSecondDrain),
                 static_cast<unsigned>(containsBeforeSecondDrain), secondDrainCount, secondCount,
                 sizeAfterSecondDrain, secondConsume.work, secondConsume.consumedLedger,
                 secondConsume.stats.consumed, secondConsume.stats.skippedNotHeap,
                 secondConsume.stats.skippedWeak, sizeAfterSecondConsume,
#if defined(MRT_GC_UNIT_TESTS)
                 static_cast<unsigned>(fx.region1->IsYoungRegion()), static_cast<size_t>(firstReceipt.seen),
                 static_cast<size_t>(firstReceipt.consumed), static_cast<size_t>(firstReceipt.stale),
                 static_cast<size_t>(firstReceipt.deadHolder), static_cast<size_t>(firstReceipt.noOrigin),
                 static_cast<size_t>(firstReceipt.badTarget)
#else
                 static_cast<unsigned>(fx.region1->IsYoungRegion())
#endif
                 );
    std::fflush(stderr);

    GC_EXPECT_TRUE(firstCount == 1);
    GC_EXPECT_EQ(sizeAfterFirstDrain, 0u);
    GC_EXPECT_TRUE(sizeAfterFirstConsume == 1);
#if defined(MRT_GC_UNIT_TESTS)
    GC_EXPECT_EQ(firstReceipt.seen, 1u);
    GC_EXPECT_EQ(firstReceipt.consumed, 1u);
#endif
    GC_EXPECT_TRUE(containsBeforeSecondDrain);
    GC_EXPECT_TRUE(secondCount == 1);
    GC_EXPECT_EQ(sizeAfterSecondDrain, 0u);
    GC_EXPECT_TRUE(sizeAfterSecondConsume == 1);

    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// r6b positive control: if the product consumer were absent, the existing
// post-store exit would still register the bare store. Cutting the consumer's
// re-arm line must leave this item green, so the negative arm is precise.
GC_OTHER_VM_TEST(Remset, PostStoreControlRegistersAfterDrain)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    fx.region1->SetYoungAge(1);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    BaseObject* objectB = fx.PlaceObject(fx.heapStart + RegionInfo::UNIT_SIZE + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(objectB) + 64);

    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    Barrier barrier(collector, rs);
    InstalledBarrierScope installedBarrier(barrier);

    field->StoreColoured(zpointer::null);
    barrier.WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> firstMinor;
    const size_t firstDrainCount = rs.DrainForMinor(firstMinor);
    const size_t firstCount = firstMinor.count(slot);
    const size_t sizeAfterFirstDrain = rs.Size();

    RefField<> taggedB = RemsetRearmTestAccess::Tag(collector, objectB);
    const uintptr_t observedPrev = raw(field->GetFieldValue());
    field->StoreColoured(taggedB.GetFieldValue());
    const uintptr_t fieldBeforeHook = raw(field->GetFieldValue());
    const bool containsBeforeHook = rs.Contains(slot);
    CJ_MCC_PostWriteRefField(objectB, fx.obj0, field, observedPrev);
    const uintptr_t fieldAfterHook = raw(field->GetFieldValue());
    const bool containsAfterHook = rs.Contains(slot);
    const size_t sizeAfterHook = rs.Size();
    std::unordered_set<MAddress> controlMinor;
    const size_t controlDrainCount = rs.DrainForMinor(controlMinor);
    const size_t controlCount = controlMinor.count(slot);
    const size_t sizeAfterControlDrain = rs.Size();

    std::fprintf(stderr,
                 "DETAIL arm=post_store_control slot=0x%zx first_drain=%zu first_count=%zu "
                 "size_after_first_drain=%zu field_before_hook=0x%zx remset_before_hook=%u "
                 "field_after_hook=0x%zx remset_after_hook=%u size_after_hook=%zu "
                 "control_drain=%zu control_count=%zu size_after_control_drain=%zu "
                 "invariant=post_store_registers\n",
                 static_cast<size_t>(slot), firstDrainCount, firstCount, sizeAfterFirstDrain,
                 static_cast<size_t>(fieldBeforeHook), static_cast<unsigned>(containsBeforeHook),
                 static_cast<size_t>(fieldAfterHook), static_cast<unsigned>(containsAfterHook), sizeAfterHook,
                 controlDrainCount, controlCount, sizeAfterControlDrain);
    std::fflush(stderr);

    GC_EXPECT_TRUE(firstCount == 1);
    GC_EXPECT_EQ(sizeAfterFirstDrain, 0u);
    GC_EXPECT_FALSE(containsBeforeHook);
    GC_EXPECT_TRUE(containsAfterHook);
    GC_EXPECT_TRUE(controlCount == 1);
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
    TestCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    InstalledBarrierScope installedBarrier(barrier);

    const uintptr_t plain = reinterpret_cast<uintptr_t>(fx.obj0);
    std::memcpy(field, &plain, sizeof(plain));
    GC_EXPECT_TRUE(collector.is_store_good(*field));
    GC_EXPECT_EQ(ClassifySlotWord(plain), SlotWordVerdict::kIllegal);
    RefField<> installed = collector.GetAndTryTagRefField(fx.obj1);
    field->StoreColoured(installed.GetFieldValue());
    GC_EXPECT_FALSE(rs.Contains(slot)); // positive control: the direct store alone does not record

    CJ_MCC_PostWriteRefField(fx.obj1, fx.obj0, field, plain);
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
    TestCollector collector;
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
    const uintptr_t observedPrev = raw(field->GetFieldValue());
    field->StoreColoured(installed.GetFieldValue());
    GC_EXPECT_FALSE(rs.Contains(slot)); // pre-fix compiler hit result

    CJ_MCC_PostWriteRefField(fx.obj1, fx.obj0, field, observedPrev);
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
    TestCollector collector;
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
    TestCollector collector;
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
    TestCollector collector;
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
