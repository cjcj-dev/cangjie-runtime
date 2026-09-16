// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Remembered-set producer/consumer tests. ZGC zBarrier.inline.hpp:695-733
// selects the slow path from the previous colour and remembers old heap slots;
// zRemembered.cpp:578-589 re-registers scanned slots whose target remains young.

#include "gc_cycle_sequence_fixture.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_set>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// RememberedSet::Record is private. The minor's consumer
// (WCollector::RescanRememberedSet) calls Record
// directly when it re-arms a scanned slot, so a test that cannot call it cannot model the re-arm at
// all.  Same idiom the fixture already uses for ZPage; scoped to this one header.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif
#define private public
#include "Heap/z/zRememberedSet.hpp"
#undef private

#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"
#include "Mutator/ThreadLocal.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

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

    static GenerationCycle& YoungCycle(WCollector& collector)
    {
        return collector.youngCycle;
    }

    static RefField<> Tag(WCollector& collector, BaseObject* object)
    {
        return collector.GetAndTryTagRefField(object);
    }

    static void BeginMinor(WCollector& collector)
    {
        // This remains a synthetic remset component fixture, not a mark-start
        // acceptance test. Bind its actual collector/domain/phase before the
        // linked product barrier can call ZGeneration::mark_object (:118-122).
        if (collector.youngCycle.Workers() == nullptr) {
            GcHeapFixture::AdoptGenerationIdentity(collector, Heap::GetHeap().GetCollector());
            collector.youngCycle.InitializeWorkers(1);
            collector.StartYoungMarkWork();
            collector.youngCycle.Begin(0);
        }
        collector.youngCycle.PublishPhase(GC_PHASE_TRACE);
        ZGlobalsPointers::flip_young_mark_start();
    }

    static bool FixInteriorSlot(WCollector& collector, RefField<>& field, BaseObject* knownBase)
    {
        return collector.FixMinorEvacuatedSlot(field, knownBase, nullptr);
    }

    static ConsumeResult ConsumePrevious(WCollector& collector, const std::unordered_set<MAddress>& previous,
                                          BaseObject* currentMinorRoot)
    {
        WorkStack workStack = collector.NewWorkStack();
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
        // Mark work now belongs to the generation domain, not the obsolete
        // caller staging vector. Only dispose fixture-owned pending work here;
        // real follow/termination is covered by p2FieldBarrierExercise.
        auto& domain = *collector.YoungMark();
        auto& stacks = domain.Stacks();
        const size_t work = stacks.Population();
        for (size_t stripe = 0; stripe < domain.Stripes().NStripes(); ++stripe) {
            if (auto* stack = stacks.StealLocal(stripe)) MarkStripeStack::Destroy(stack);
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
    const uintptr_t desired = raw(ZAddress::color(static_cast<zaddress>(reinterpret_cast<uintptr_t>(interior)), static_cast<uintptr_t>(::g_cjStoreGoodMask)));
    // Change only the remembered epoch.  The word remains load/mark-good, so
    // ResolveMinorReference returns the payload without rewriting the slot;
    // the interior StoreGood publication below is therefore the sole repair.
    const uintptr_t initial = desired ^ ZPointerRememberedMask;
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

// These standalone native threads have no CJ scheduler. Identify them as
// runtime threads so GetMutator reads the product TLS. With no mutator, the
// store barrier uses the direct mark/remember arm (ZGC zBarrier.cpp:253-261).
class RemsetNativeThreadScope final {
public:
    RemsetNativeThreadScope() : previous(ThreadLocal::GetThreadType())
    {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    }
    ~RemsetNativeThreadScope() { ThreadLocal::SetThreadType(previous); }
private:
    ThreadType previous;
};

// ZBarrier::store_barrier_on_heap_oop_field (zBarrier.inline.hpp:695-706):
// a previous-epoch, non-null slot takes the ordinary store slow path. Raw null
// and store-good slots intentionally do not. Keep all other colour families good.
zpointer PreviousRememberedPointer(BaseObject* object)
{
    return to_zpointer(raw(GcUnit::StoreGoodPointer(object)) ^ ZPointerRememberedMask);
}

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

// U7: product Barrier NVI WriteReference records old→young edge.
GC_TEST(Remset, OldToYoungRecordedByBarrier)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(Heap::GetHeap().GetRememberedSet(), reinterpret_cast<MAddress>(field)));
}

// ZGC zBarrier.inline.hpp:695-706: store-good fast path, old epoch slow path.
GC_TEST(Remset, StoreGoodSkipsAndPreviousEpochRecords)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    GC_EXPECT_EQ(ClassifySlotWord(reinterpret_cast<uintptr_t>(fx.obj1)), SlotWordVerdict::kIllegal);

    field->StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));
    GC_EXPECT_TRUE(ZPointer::is_store_good((*field).GetFieldValue()));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

// ZGC zRemembered.cpp:591 and zBarrier.inline.hpp:695: bitmap and colour epochs differ.
GC_TEST(Remset, StoreGoodRewriteRequiresEpochChangeAfterDrain)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> firstMinor;
    Heap::GetHeap().GetRememberedSet().DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(slot) == 1);
    GC_EXPECT_EQ(Heap::GetHeap().GetRememberedSet().Size(), 0u);
    GC_EXPECT_TRUE(ZPointer::is_store_good((*field).GetFieldValue()));

    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);

    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));
    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

// Product-path form of the r6b failure arm. Generation drains the previous
// face, then WCollector::RescanRememberedSet consumes it and re-arms the slot
// while its resolved target is still young (zRemembered.cpp:578-589). The
// compiler-like bare store below makes no runtime call; minor #2 must still
// receive the slot from the current face established by that product consumer.
GC_OTHER_VM_TEST(Remset, StoreGoodAfterProductConsumerRearm)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    // Product remset consumption accepts exact object starts through the loaded
    // TypeInfo registry (Remembered.cpp:1157-1204). GcHeapFixture normally needs
    // residence only; this product-entry test needs the stronger real-object precondition.
    fx.typeInfo->SetUUID(1);
    TypeInfoManager::GetTypeInfoManager().AddTypeInfo(fx.typeInfo);
    const bool targetTypeRegistered = TypeInfoManager::GetTypeInfoManager().ContainsTypeInfo(fx.typeInfo);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    BaseObject* objectB = fx.PlaceObject(fx.heapStart + ZPage::UNIT_SIZE + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(objectB) + 64);

    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
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
#endif
    const size_t sizeAfterFirstConsume = rs.Size();

    // LLVM store-good hit analogue: coloured volatile store, no runtime hand-off.
    RefField<> taggedB = RemsetRearmTestAccess::Tag(collector, objectB);
    field->StoreColoured(taggedB.GetFieldValue());
    const uintptr_t fieldBeforeSecondDrain = raw(field->GetFieldValue());
    const bool containsBeforeSecondDrain = Heap::GetHeap().GetRememberedSet().Contains(slot);
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
}

// r6b positive control: if the product consumer were absent, the existing
// post-store exit would still register the bare store. Cutting the consumer's
// re-arm line must leave this item green, so the negative arm is precise.
GC_OTHER_VM_TEST(Remset, PostStoreControlRegistersAfterDrain)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    BaseObject* objectB = fx.PlaceObject(fx.heapStart + ZPage::UNIT_SIZE + 128);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(objectB) + 64);

    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    std::unordered_set<MAddress> firstMinor;
    const size_t firstDrainCount = rs.DrainForMinor(firstMinor);
    const size_t firstCount = firstMinor.count(slot);
    const size_t sizeAfterFirstDrain = rs.Size();

    RefField<> taggedB = RemsetRearmTestAccess::Tag(collector, objectB);
    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    const uintptr_t fieldBeforeHook = raw(field->GetFieldValue());
    const bool containsBeforeHook = Heap::GetHeap().GetRememberedSet().Contains(slot);
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(field), false);
    const uintptr_t fieldAfterHook = raw(field->GetFieldValue());
    const bool containsAfterHook = Heap::GetHeap().GetRememberedSet().Contains(slot);
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

// ZGC zBarrier.inline.hpp:695-706: the compiler hand-off follows the previous word.
GC_TEST(Remset, CompilerPostStoreSkipsGoodAndRecordsPreviousEpoch)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    GC_EXPECT_EQ(ClassifySlotWord(reinterpret_cast<uintptr_t>(fx.obj1)), SlotWordVerdict::kIllegal);

    const uintptr_t good = raw(GcUnit::StoreGoodPointer(fx.obj0));
    field->StoreColoured(GcUnit::StoreGoodPointer(fx.obj1));
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(field), false);
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));
    field->StoreColoured(PreviousRememberedPointer(fx.obj0));
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(field), false);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

// ZGC zBarrier.inline.hpp:729-733: remember tests slot generation on the slow path.
GC_TEST(Remset, CompilerPostStoreFastPathIgnoresNewTargetGeneration)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::old);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj0);
    GC_EXPECT_TRUE(ZPointer::is_store_good((*field).GetFieldValue()));
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));

    fx.region1->reset(PageAge::eden);
    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));

    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(field), false);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(Remset, AtomicWriteRecordsOldToYoung)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    ZBarrier::AtomicWriteReference(fx.obj0, *field, fx.obj1, std::memory_order_seq_cst);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(Remset, AtomicSwapRecordsOldToYoung)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    BaseObject* old = ZBarrier::AtomicSwapReference(fx.obj0, *field, fx.obj1, std::memory_order_seq_cst);
    GC_EXPECT_TRUE(old == nullptr);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(Remset, CompareAndSwapRemembersBeforeAttempt)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    auto* field = &HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    GC_EXPECT_FALSE(ZBarrier::CompareAndSwapReference(fx.obj0, *field, fx.obj1, fx.obj1,
                                                    std::memory_order_seq_cst,
                                                    std::memory_order_seq_cst));
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
    GC_EXPECT_TRUE(to_object(field->GetTargetObject()) == nullptr);

    GC_EXPECT_TRUE(ZBarrier::CompareAndSwapReference(fx.obj0, *field, nullptr, fx.obj1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst));
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

// ZGC zBarrier.inline.hpp:695-733: old heap slow-path stores remember during idle too.
GC_TEST(Remset, IdleBarrierOldToYoungRecorded)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(Heap::GetHeap().GetRememberedSet(), reinterpret_cast<MAddress>(field)));
}

// Static roots are enumerated directly by every minor and must not enter the
// heap-only remembered set. Restoring the external record path makes this red.
GC_TEST(Remset, StaticRootNotRecorded)
{
    GcHeapFixture fx;
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);
    NativeSlot root(zpointer::null);

    ZBarrier::WriteStaticRef(root, fx.obj1);
    std::unordered_set<MAddress> records;
    rs.DrainForMinor(records);
    GC_EXPECT_EQ(records.size(), 0u);
}

// U7: young→young must NOT enter remset (only old→young).
GC_TEST(Remset, YoungToYoungNotRecorded)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::eden);
    fx.region0->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(zpointer::null);
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
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
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::old);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);
    GC_EXPECT_TRUE(ExpectRecorded(Heap::GetHeap().GetRememberedSet(), reinterpret_cast<MAddress>(field)));
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
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);

    std::unordered_set<MAddress> firstMinor;
    Heap::GetHeap().GetRememberedSet().DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(reinterpret_cast<MAddress>(field)) == 1);

    // No second write: the edge is still in the heap, the record is not.
    std::unordered_set<MAddress> secondMinor;
    Heap::GetHeap().GetRememberedSet().DrainForMinor(secondMinor);
    GC_EXPECT_EQ(secondMinor.size(), 0u);
}

GC_TEST(Remset, ReRecordWhileConsumingLandsInTheNextCycleBuffer)
{
    RemsetNativeThreadScope nativeThread;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    fx.region1->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZPage::UNIT_SIZE);

    field->StoreColoured(PreviousRememberedPointer(fx.obj1));
    ZBarrier::WriteReference(fx.obj0, *field, fx.obj1);

    auto& heapRs = Heap::GetHeap().GetRememberedSet();
    std::unordered_set<MAddress> firstMinor;
    heapRs.DrainForMinor(firstMinor);
    GC_EXPECT_TRUE(firstMinor.count(slot) == 1);
    GC_EXPECT_EQ(heapRs.Size(), 0u);

    heapRs.Record(slot);
    heapRs.Record(slot);
    GC_EXPECT_EQ(heapRs.Size(), 1u);

    std::unordered_set<MAddress> secondMinor;
    heapRs.DrainForMinor(secondMinor);
    GC_EXPECT_TRUE(secondMinor.count(slot) == 1);
    GC_EXPECT_EQ(secondMinor.size(), 1u);

    // And it self-drains: a cycle that does not re-arm gives the slot up, which is how an edge whose
    // target has been promoted out of young stops costing a scan.
    std::unordered_set<MAddress> thirdMinor;
    heapRs.DrainForMinor(thirdMinor);
    GC_EXPECT_EQ(thirdMinor.size(), 0u);
}

// zRemembered.cpp:347-420: enumerate sparse pages, tolerate a stale page
// entry, and keep post-flip publications separate from previous-face scanning.
GC_TEST(Remset, SparsePagesConsumeAndRearmAcrossFaces)
{
    const MAddress start = 0x300000000ULL;
    const size_t pageBytes = ZPage::UNIT_SIZE;
    RememberedSet rs;
    rs.Initialize(start, 130 * pageBytes);
    const MAddress first = start + pageBytes + sizeof(RefField<>);
    const MAddress stale = start + 65 * pageBytes + sizeof(RefField<>);
    const MAddress last = start + 129 * pageBytes + sizeof(RefField<>);
    rs.Record(first);
    rs.Record(stale);
    rs.Record(last);
    rs.ClearRegion(start + 65 * pageBytes, start + 66 * pageBytes);
    std::unordered_set<size_t> pages;
    rs.VisitRememberedPages(rs.activeBuffer.load(), [&](size_t page) { pages.insert(page); });
    const std::unordered_set<size_t> expectedPages{1, 65, 129};
    GC_EXPECT_TRUE(pages == expectedPages);
    rs.FlipForMinor();
    rs.Record(stale);
    std::unordered_set<MAddress> previous;
    rs.ScanPreviousForMinor(previous);
    const std::unordered_set<MAddress> expectedPrevious{first, last};
    GC_EXPECT_TRUE(previous == expectedPrevious);
    GC_EXPECT_TRUE(rs.Contains(stale));
    rs.FlipForMinor();
    previous.clear();
    rs.ScanPreviousForMinor(previous);
    const std::unordered_set<MAddress> expectedNext{stale};
    GC_EXPECT_TRUE(previous == expectedNext);
}

#if defined(MRT_GC_UNIT_TESTS)
namespace {

std::unordered_set<MAddress> MakeFlipSlots(MAddress start, size_t capacity, size_t count, size_t phase)
{
    const size_t available = capacity / sizeof(RefField<>);
    const size_t stride = available / count;
    std::unordered_set<MAddress> slots;
    slots.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t bit = (phase + i * stride) % available;
        slots.insert(start + bit * sizeof(RefField<>));
    }
    return slots;
}

void RecordFlipSlots(RememberedSet& rememberedSet, const std::unordered_set<MAddress>& slots)
{
    for (MAddress slot : slots) {
        rememberedSet.Record(slot);
    }
}

enum class RemsetWordBacking : uint8_t {
    BITMAP,
    PAGE_MAP,
};

enum class RemsetProbeOperation : uint8_t {
    FLIP,
    CLEAR_ACTIVE,
};

size_t ProbeRemsetWordAccess(RememberedSet& rememberedSet, RemsetWordBacking backing,
                             RemsetProbeOperation operation)
{
#if defined(__linux__)
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            _exit(120);
        }
        void* sentinel = mmap(nullptr, static_cast<size_t>(pageSize), PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sentinel == MAP_FAILED) {
            _exit(121);
        }
        auto* words = static_cast<std::atomic<uint64_t>*>(sentinel);
        for (size_t buffer = 0; buffer < RememberedSet::kBufferCount; ++buffer) {
            if (backing == RemsetWordBacking::BITMAP) {
                (void)rememberedSet.bitmaps[buffer].release();
                rememberedSet.bitmaps[buffer].reset(words);
            } else {
                (void)rememberedSet.rememberedPages[buffer].release();
                rememberedSet.rememberedPages[buffer].reset(words);
            }
        }

        if (operation == RemsetProbeOperation::FLIP) {
            const size_t before = rememberedSet.activeBuffer.load(std::memory_order_relaxed);
            rememberedSet.FlipForMinor();
            const size_t after = rememberedSet.activeBuffer.load(std::memory_order_relaxed);
            _exit(after == (before ^ 1U) ? 42 : 122);
        } else {
            const size_t active = rememberedSet.activeBuffer.load(std::memory_order_relaxed);
            const size_t removed = rememberedSet.ClearBuffer(active);
            _exit(removed != 0 ? 42 : 122);
        }
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    GC_EXPECT_EQ(waited, child);
    // Exit 42 proves the invoked operation reached its publication/removal
    // postcondition without touching the protected backing.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        return 0;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        // The first trapped access is sufficient because the invariant is zero
        // backing-word accesses, independent of capacity or representation.
        return 1;
    }
    GC_EXPECT_TRUE(false);
    return 0;
#else
    (void)rememberedSet;
    (void)backing;
    (void)operation;
    GC_EXPECT_TRUE(false);
    return 0;
#endif
}

RememberedSet::FlipTouchCounts ProbeFlipWordAccesses(RememberedSet& rememberedSet)
{
    return RememberedSet::FlipTouchCounts {
        ProbeRemsetWordAccess(rememberedSet, RemsetWordBacking::BITMAP, RemsetProbeOperation::FLIP),
        ProbeRemsetWordAccess(rememberedSet, RemsetWordBacking::PAGE_MAP, RemsetProbeOperation::FLIP),
    };
}

} // namespace

// Positive control for the backing-word sentinel. A real ClearBuffer over a
// non-empty face must fault against both protected backings; otherwise a zero
// reported by the O(1) guard below would not be evidence.
GC_TEST(Remset, FlipTouchReceiptPositiveControl)
{
    constexpr MAddress start = 0x100000000ULL;
    constexpr size_t capacity = 64 * 1024;
    RememberedSet rs;
    rs.Initialize(start, capacity);
    rs.Record(start);

    const size_t bitmapAccesses = ProbeRemsetWordAccess(
        rs, RemsetWordBacking::BITMAP, RemsetProbeOperation::CLEAR_ACTIVE);
    const size_t dirtyAccesses = ProbeRemsetWordAccess(
        rs, RemsetWordBacking::PAGE_MAP, RemsetProbeOperation::CLEAR_ACTIVE);
    std::fprintf(stderr, "DETAIL remset_flip_touch_control bitmap_word_accesses=%zu "
                         "dirty_word_accesses=%zu\n",
                 bitmapAccesses, dirtyAccesses);
    GC_EXPECT_EQ(bitmapAccesses, 1u);
    GC_EXPECT_EQ(dirtyAccesses, 1u);
    GC_EXPECT_EQ(rs.Size(), 1u);
}

// Two bitmap capacities x two cardinalities. Flip itself must touch no bitmap
// or page-map word. The post-flip producer writes before the deferred consumer
// runs, so exact set equality on both faces also proves that the consumer reads
// the pre-flip contents and does not mix in records from the new current face.
GC_TEST(Remset, FlipIsConstantTimeAndPreservesFaceEpochs)
{
    struct FlipCase {
        size_t capacity;
        size_t preCount;
        size_t postCount;
    };
    constexpr MAddress start = 0x200000000ULL;
    const FlipCase cases[] = {
        { 64 * 1024, 1, 2 },
        { 64 * 1024, 257, 131 },
        { 8 * 1024 * 1024, 1, 2 },
        { 8 * 1024 * 1024, 257, 131 },
    };

    for (const FlipCase& testCase : cases) {
        RememberedSet rs;
        rs.Initialize(start, testCase.capacity);
        const auto pre = MakeFlipSlots(start, testCase.capacity, testCase.preCount, 0);
        const auto post = MakeFlipSlots(start, testCase.capacity, testCase.postCount, 1);
        RecordFlipSlots(rs, pre);

        const auto firstFlip = ProbeFlipWordAccesses(rs);
        rs.FlipForMinor();
        RecordFlipSlots(rs, post);
        std::unordered_set<MAddress> previous;
        const size_t previousCount = rs.ScanPreviousForMinor(previous);
        const auto current = rs.Snapshot();
        std::fprintf(stderr,
                     "DETAIL remset_flip capacity=%zu pre=%zu post=%zu first_bitmap_word_accesses=%zu "
                     "first_dirty_word_accesses=%zu previous=%zu current=%zu\n",
                     testCase.capacity, testCase.preCount, testCase.postCount, firstFlip.bitmapWords,
                     firstFlip.dirtyWords, previousCount, current.size());
        GC_EXPECT_EQ(firstFlip.bitmapWords, 0u);
        GC_EXPECT_EQ(firstFlip.dirtyWords, 0u);
        GC_EXPECT_EQ(previousCount, testCase.preCount);
        GC_EXPECT_TRUE(previous == pre);
        GC_EXPECT_EQ(current.size(), testCase.postCount);
        GC_EXPECT_TRUE(current == post);

        // The first deferred consumer cleared the face that becomes current
        // here. The second consumer must return exactly the post-flip set,
        // leaving neither pre-flip residue nor current-face residue behind.
        const auto secondFlip = ProbeFlipWordAccesses(rs);
        rs.FlipForMinor();
        std::unordered_set<MAddress> secondPrevious;
        const size_t secondPreviousCount = rs.ScanPreviousForMinor(secondPrevious);
        const auto secondCurrent = rs.Snapshot();
        std::fprintf(stderr,
                     "DETAIL remset_flip_reuse capacity=%zu pre=%zu post=%zu second_bitmap_word_accesses=%zu "
                     "second_dirty_word_accesses=%zu previous=%zu current=%zu\n",
                     testCase.capacity, testCase.preCount, testCase.postCount, secondFlip.bitmapWords,
                     secondFlip.dirtyWords, secondPreviousCount, secondCurrent.size());
        GC_EXPECT_EQ(secondFlip.bitmapWords, 0u);
        GC_EXPECT_EQ(secondFlip.dirtyWords, 0u);
        GC_EXPECT_EQ(secondPreviousCount, testCase.postCount);
        GC_EXPECT_TRUE(secondPrevious == post);
        GC_EXPECT_TRUE(secondCurrent.empty());
    }
}
#endif

// zGeneration.cpp:871-880: preparing a request changes neither sequence nor
// face; every mark-start advances both, even when the remembered set is empty.
GC_TEST(Remset, YoungMarkStartAdvancesSequenceAndFlipsTogether)
{
    alignas(8) uint64_t storage[16] {};
    RememberedSet rs;
    rs.Initialize(reinterpret_cast<MAddress>(storage), sizeof(storage));
    GenerationCycle young(GCCycleGeneration::YOUNG);
    GenerationCycle old(GCCycleGeneration::OLD);
    for (uint64_t cycle = 0; cycle != 4; ++cycle) {
        const uint64_t sequence = young.Sequence();
        const uint8_t face = rs.activeBuffer.load(std::memory_order_acquire);
        old.RecordYoungSequenceAtRelocateStart(sequence);
        young.Begin(cycle + 1);
        GC_EXPECT_EQ(young.Sequence(), sequence);
        GC_EXPECT_EQ(rs.activeBuffer.load(std::memory_order_acquire), face);
        GenerationSequenceFixture::AdvanceYoung(young, rs);
        GC_EXPECT_EQ(young.Sequence(), sequence + 1);
        GC_EXPECT_EQ(rs.activeBuffer.load(std::memory_order_acquire), face ^ 1U);
        GC_EXPECT_FALSE(old.ActiveRemsetIsCurrent(young.Sequence()));
        std::unordered_set<MAddress> previous;
        rs.ScanPreviousForMinor(previous);
        GC_EXPECT_TRUE(previous.empty());
        young.End();
    }
}

// zRelocate.cpp:698-725: the real relocation entry captures a face and the
// product transfer reads that face, then remembers the relocated field in current.
GC_OTHER_VM_TEST(Remset, OldRelocationSelectsCapturedFaceAcrossFlips)
{
    GcHeapFixture heap;
    auto& collector = static_cast<WCollector&>(Heap::GetHeap().GetCollector());
    GenerationCycle& young = RemsetRearmTestAccess::YoungCycle(collector);
    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    // The fixture may leave its liveness-setup cycle active. Complete that
    // setup before issuing the first independent mark-start request.
    if (young.Snapshot().active) young.End();
    auto markStart = [&] {
        young.Begin(young.Sequence() + 1);
        GenerationSequenceFixture::AdvanceYoung(young, rs);
        young.End();
    };
    const MAddress from = heap.heapStart + 256;
    const MAddress to = heap.heapStart + 128;
    collector.PublishGenerationPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
    size_t checked = 0;
    for (uint8_t initial = 0; initial != 2; ++initial) {
        for (size_t flips = 0; flips != 4; ++flips) {
            rs.ClearRegion(heap.heapStart, heap.heapStart + 2 * ZPage::UNIT_SIZE);
            if (rs.activeBuffer.load() != initial) markStart();
            collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
            collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
            rs.Record(from + sizeof(void*));
            for (size_t flip = 0; flip != flips; ++flip) markStart();
            // FORWARD is the same relocation: it must not replace the snapshot.
            collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
            GC_EXPECT_EQ(collector.OldActiveRemsetIsCurrent(), flips % 2 == 0);
            GC_EXPECT_EQ(rs.TransferObjectSlots(from, to, 32), 1u);
            GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(to + sizeof(void*)));
            ++checked;
            std::fprintf(stderr, "DETAIL remset_face initial=%u flips=%zu assertions_executed=1\n",
                         static_cast<unsigned>(initial), flips);
        }
    }
    GC_EXPECT_EQ(checked, 8u);
}

GC_OTHER_VM_TEST(Remset, RelocatedFieldsEnterCurrentOutsideYoungMark)
{
    GcHeapFixture heap;
    auto& collector = Heap::GetHeap().GetCollector();
    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    collector.PublishGenerationPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_IDLE);
    collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_IDLE);
    collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_PREFORWARD);
    const MAddress from = heap.heapStart + 256;
    const MAddress to = heap.heapStart + 128;
    rs.Record(from + sizeof(void*));
    GC_EXPECT_EQ(rs.TransferObjectSlots(from, to, 32), 1u);
    rs.FlipForMinor();
    std::unordered_set<MAddress> fields;
    rs.ScanPreviousForMinor(fields);
    GC_EXPECT_TRUE(fields.count(to + sizeof(void*)) == 1);
}

GC_OTHER_VM_TEST(Remset, InPlacePreviousFieldsPublishDuringYoungMark)
{
    GcHeapFixture heap;
    auto& collector = Heap::GetHeap().GetCollector();
    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    collector.PublishGenerationPhase(GCCycleGeneration::YOUNG, GCPhase::GC_PHASE_TRACE);
    collector.PublishGenerationPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_FORWARD);
    const MAddress from = heap.heapStart + 256;
    const MAddress to = heap.heapStart + 128;
    rs.Record(from + sizeof(void*));
    rs.FlipForMinor();
    auto* fwd = ZForwarding::Create(1, heap.heapStart, heap.heapStart, ZPage::UNIT_SIZE);
    fwd->set_in_place();
    const size_t moved = rs.TransferObjectSlots(from, to, 32, fwd);
    fwd->relocated_remembered_fields_after_relocate();
    fwd->release_page();
    fwd->mark_done();
    size_t visited = 0;
    MAddress field = 0;
    fwd->relocated_remembered_fields_apply_to_published([&](MAddress p) { ++visited; field = p; });
    fwd->Destroy();
    GC_EXPECT_EQ(moved, 1u);
    GC_EXPECT_EQ(visited, 1u);
    GC_EXPECT_EQ(field, to + sizeof(void*));
}
