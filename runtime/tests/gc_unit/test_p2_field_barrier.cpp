// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "ObjectModel/MArray.inline.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo*, MIndex);
extern "C" ArrayRef MCC_NewArray(const TypeInfo*, MIndex);
}
using namespace MapleRuntime;
namespace {
std::atomic<unsigned> failures{0};
std::atomic<unsigned> finalized{0};
extern "C" void P2Finalize(BaseObject*, TypeInfo*) { ++finalized; }
std::mutex resultMutex;
void Expect(bool value, const char* name)
{
    std::printf("P2_ASSERT %s %s\n", name, value ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!value) ++failures;
}
TypeInfo* Type(unsigned char* storage, bool refs, unsigned fields)
{
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(fields * sizeof(uintptr_t));
    if (refs) {
        type->SetFlagHasRefField();
        GCTib tib{};
        tib.tag = SIGN_BIT | ((uintptr_t(1) << fields) - 1);
        type->SetGCTib(tib);
    }
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(TypeInfo));
    return type;
}
RefField<>& Slot(BaseObject* object, unsigned index = 0)
{
    return HeapSlotAt<>(reinterpret_cast<MAddress>(object) + sizeof(uintptr_t) * (index + 1));
}
}

// The compiler-generated managed caller owns runtime startup. Inputs use real
// allocation/store/export APIs; no phase, mark, forwarding or remset state is seeded.
extern "C" int p2FieldBarrierExercise()
{
    alignas(TypeInfo) static unsigned char types[4][sizeof(TypeInfo)]{};
    auto* holderType = Type(types[0], true, 2);
    auto* edgeType = Type(types[1], true, 2);
    auto* leafType = Type(types[2], false, 1);
    auto* finalType = Type(types[3], true, 2);
    // With zero type arguments the public metadata union holds the finalizer
    // entry rather than a generic source; this is input TypeInfo, not GC state.
    finalType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto& barrier = Heap::GetBarrier();
    auto* holder = MObject::NewPinnedObject(holderType, 24);
    auto* oldChild = MObject::NewPinnedObject(leafType, 16);
    const U64 root = heap.RegisterExportRoot(holder);
    barrier.WriteReference(holder, Slot(holder), oldChild);
    barrier.WriteReference(holder, Slot(holder, 1), oldChild);
    const bool finalizableCase = std::getenv("P2_FINALIZABLE") != nullptr;
    BaseObject* finalHolder = nullptr;
    BaseObject* finalOld = nullptr;
    BaseObject* upgraded = nullptr;
    U64 upgradeRoot = 0;
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    const auto discoveredBefore = references.Discovered(ReferenceType::FINAL);
    const auto enqueuedBefore = references.Enqueued(ReferenceType::FINAL);
    if (finalizableCase) {
        finalHolder = MObject::NewPinnedObject(finalType, 24);
        finalOld = MObject::NewPinnedObject(edgeType, 24);
        auto* finalSentinel = MObject::NewPinnedObject(leafType, 16);
        barrier.WriteReference(finalOld, Slot(finalOld), finalSentinel);
        barrier.WriteReference(finalHolder, Slot(finalHolder), oldChild);
        barrier.WriteReference(finalHolder, Slot(finalHolder, 1), finalOld);
        finalHolder->OnFinalizerCreated();
        upgraded = MObject::NewPinnedObject(finalType, 24);
        barrier.WriteReference(upgraded, Slot(upgraded), oldChild);
        upgraded->OnFinalizerCreated();
        upgradeRoot = heap.RegisterExportRoot(upgraded);
    }
    // Advance a real young epoch before overwriting the old slot. Its previous
    // non-null word must go through the store barrier and remember the slot.
    collector.RequestGC(GC_REASON_YOUNG, false);
    auto* child = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
    auto* sentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    auto* oldViaYoung = MObject::NewPinnedObject(leafType, 16);
    barrier.WriteReference(child, Slot(child), sentinel);
    barrier.WriteReference(child, Slot(child, 1), oldViaYoung);
    barrier.WriteReference(holder, Slot(holder), child);
    if (finalizableCase) barrier.WriteReference(finalHolder, Slot(finalHolder), child);
    auto* rootedControl = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    const U64 controlRoot = heap.RegisterExportRoot(rootedControl);
    Expect(!RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder))->IsYoungRegion(), "real_holder_is_old");
    Expect(RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(child))->IsYoungRegion(), "real_child_is_young");
    unsigned finalOldOld = 0, finalOldYoung = 0, finalFollow = 0, finalYoungFast = 0;
    unsigned remsetChild = 0, oldOld = 0, oldYoung = 0, youngFollow = 0, oldYoungFast = 0;
    unsigned youngOldMajor = 0, youngOldMinor = 0;
    BaseObject* currentChild = child;
    Barrier::testFieldMarkResult = [&](Barrier::FieldMarkKind kind, RefField<>& field,
                                      zpointer observed, zaddress result) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (kind == Barrier::FieldMarkKind::Finalizable && finalHolder != nullptr) {
            Expect((collector.GetCycleSnapshot(GCCycleGeneration::OLD).phase == GC_PHASE_TRACE ||
                    collector.GetCycleSnapshot(GCCycleGeneration::OLD).phase == GC_PHASE_CLEAR_SATB_BUFFER), "finalizable_follow_during_mark");
            if (&field == &Slot(finalHolder)) {
                const bool fast = ZPointer::is_load_good(observed) && ZPointer::is_marked_any_old(observed);
                if (fast) {
                    ++finalYoungFast;
                    Expect(!is_null(result), "finalizable_young_fast_returns_current");
                } else {
                    ++finalOldYoung;
                    Expect(is_null(result), "finalizable_young_no_object_result");
                }
                Expect(field.GetFieldValue() == observed, "finalizable_young_slot_unchanged");
                if (!fast) Expect(!ZPointer::is_marked_any_old(observed), "finalizable_young_reached_bad_color");
            }
            if (&field == &Slot(finalHolder, 1)) {
                ++finalOldOld;
                Expect(to_object(result) == finalOld, "finalizable_old_returns_current");
                auto* page = RegionInfo::GetRegionInfoAt(raw(result));
                Expect(page->IsResurrectedObject(finalOld), "finalizable_old_is_finalizable");
                Expect(!page->IsMarkedObject(page->GetMarkView<Generation::Old>(), finalOld), "finalizable_old_not_strong");
                Expect(ZPointer::is_marked_finalizable(field.GetFieldValue()), "finalizable_old_slot_color");
            }
            if (&field == &Slot(finalOld)) {
                ++finalFollow;
                Expect(!is_null(result), "finalizable_old_child_followed");
            }
        }
        if (&field == &Slot(holder) && kind == Barrier::FieldMarkKind::Remset) {
            ++remsetChild;
            currentChild = to_object(result);
            Expect(currentChild != nullptr, "remset_returns_child");
            if (currentChild != nullptr) {
                auto* page = RegionInfo::GetRegionInfoAt(raw(result));
                if (page->IsYoungRegion()) {
                    Expect(page->IsMarkedObject(page->GetMarkView<Generation::Young>(), currentChild), "remset_child_marked");
                } else {
                    Expect(ZPointer::is_marked_young(field.GetFieldValue()), "remset_old_target_young_good");
                }
            }
            Expect(ZPointer::is_load_good(field.GetFieldValue()) &&
                   ZPointer::is_marked_young(field.GetFieldValue()), "remset_slot_young_good");
        }
        if (&field == &Slot(holder, 1) && kind == Barrier::FieldMarkKind::Old) {
            ++oldOld;
            Expect(to_object(result) == oldChild, "old_old_returns_current");
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(oldChild));
            Expect(page->IsMarkedObject(page->GetMarkView<Generation::Old>(), oldChild), "old_old_marked");
        }
        if (&field == &Slot(holder) && kind == Barrier::FieldMarkKind::Old) {
            if (ZPointer::is_mark_good(observed)) {
                ++oldYoungFast;
                Expect(!is_null(result), "old_young_fast_returns_current");
                Expect(field.GetFieldValue() == observed, "old_young_fast_slot_unchanged");
            } else {
                ++oldYoung;
                Expect(is_null(result), "old_young_no_object_result");
                Expect(field.GetFieldValue() == observed, "old_young_slot_unchanged");
            }
        }
        if (currentChild != nullptr && &field == &Slot(currentChild, 1) && kind == Barrier::FieldMarkKind::Young) {
            const bool major = collector.GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots();
            if (major) ++youngOldMajor; else ++youngOldMinor;
            Expect(to_object(result) == oldViaYoung, "young_old_returns_current");
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(oldViaYoung));
            auto* bitmap = page->GetMarkBitmap(page->GetMarkView<Generation::Old>());
            const bool marked = bitmap != nullptr && bitmap->IsMarked(page->GetAddressOffset(reinterpret_cast<MAddress>(oldViaYoung)));
            Expect(marked == major, major ? "young_old_major_marks" : "young_old_minor_does_not_mark");
            Expect(ZPointer::is_store_good(field.GetFieldValue()), "young_old_heals_store_good");
        }
        if (currentChild != nullptr && &field == &Slot(currentChild) && kind == Barrier::FieldMarkKind::Young) {
            ++youngFollow;
            Expect(!is_null(result), "young_child_follows_sentinel");
            Expect(!ZPointer::is_store_bad(field.GetFieldValue()), "young_field_store_good");
        }
    };
    const bool minorOnly = std::getenv("P2_MINOR_ONLY") != nullptr;
    // Read the actual bitmap before relocation can make an unmarked target
    // unavailable. These are the target invariants, not existence guards.
    TracingCollector::testYoungMarkCompleted = [&] {
        auto* childPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(child));
        auto* sentinelPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(sentinel));
        auto* controlPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(rootedControl));
        Expect(childPage->IsMarkedObject(childPage->GetMarkView<Generation::Young>(), child),
               "remset_retains_child_first_cycle");
        Expect(sentinelPage->IsMarkedObject(sentinelPage->GetMarkView<Generation::Young>(), sentinel),
               "remset_retains_sentinel_first_cycle");
        Expect(controlPage->IsMarkedObject(controlPage->GetMarkView<Generation::Young>(), rootedControl),
               "independent_young_root_control");
    };
    collector.RequestGC(GC_REASON_YOUNG, false);
    TracingCollector::testYoungMarkCompleted = nullptr;
    if (failures.load() != 0) {
        // The target state has been observed. Do not dereference an object
        // that the faulty product has just classified as unreachable.
        Barrier::testFieldMarkResult = nullptr;
        heap.RemoveExportObject(root);
        heap.RemoveExportObject(controlRoot);
        std::printf("P2_FAST old_young=%u finalizable_young=%u\n", oldYoungFast, finalYoungFast);
        std::printf("P2_RESULT failures=%u target_stage=first_cycle\n", failures.load());
        return failures.load();
    }
    // A second cycle removes the store-buffer current-value marking side path:
    // the unchanged edge must now survive through the remembered slot itself.
    BaseObject* childBeforeNext = barrier.ReadReference(holder, Slot(holder));
    currentChild = childBeforeNext;
    BaseObject* sentinelBeforeNext = barrier.ReadReference(childBeforeNext, Slot(childBeforeNext));
    unsigned completed = 0;
    TracingCollector::testYoungMarkCompleted = [&] {
        ++completed;
        auto* childPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(childBeforeNext));
        auto* sentinelPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(sentinelBeforeNext));
        Expect(childPage->IsMarkedObject(childPage->GetMarkView<Generation::Young>(), childBeforeNext),
               "remset_retains_child_next_cycle");
        Expect(sentinelPage->IsMarkedObject(sentinelPage->GetMarkView<Generation::Young>(), sentinelBeforeNext),
               "remset_retains_sentinel_next_cycle");
    };
    collector.RequestGC(GC_REASON_YOUNG, false);
    TracingCollector::testYoungMarkCompleted = nullptr;
    Expect(completed != 0, "young_completion_observation_reached");
    if (!minorOnly) {
        // The control was needed only for remset liveness. End its real export
        // lifetime before the independent old-field scenario.
        heap.RemoveExportObject(controlRoot);
        currentChild = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
        auto* freshSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
        barrier.WriteReference(currentChild, Slot(currentChild), freshSentinel);
        barrier.WriteReference(currentChild, Slot(currentChild, 1), oldViaYoung);
        barrier.WriteReference(holder, Slot(holder), currentChild);
        if (finalizableCase) barrier.WriteReference(finalHolder, Slot(finalHolder), currentChild);
        collector.RequestGC(GC_REASON_HEU_SYNC, false);
    }
    Barrier::testFieldMarkResult = nullptr;
    if (finalizableCase && !minorOnly) {
        Expect(finalOldOld != 0, "real_finalizable_old_control_reached");
        Expect(finalOldYoung + finalYoungFast != 0, "real_finalizable_young_field_reached");
        Expect(finalFollow != 0, "real_finalizable_follow_reached");
        Expect(references.Discovered(ReferenceType::FINAL) - discoveredBefore == 2, "finalizable_discovered_once_each");
        Expect(references.Enqueued(ReferenceType::FINAL) - enqueuedBefore == 1, "finalizable_only_unupgraded_enqueued");
        auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(upgraded));
        Expect(page->IsMarkedObject(page->GetMarkView<Generation::Old>(), upgraded), "finalizable_upgraded_to_strong");
        Expect(!page->IsResurrectedObject(upgraded), "finalizable_upgraded_not_pending");
        heap.RemoveExportObject(upgradeRoot);
    }
    Expect(remsetChild != 0, "real_remset_consumer_reached");
    Expect(youngFollow != 0, "real_young_follow_reached");
    Expect(youngOldMinor != 0, "real_young_old_minor_reached");
    if (!minorOnly) {
        Expect(youngOldMajor != 0, "real_young_old_major_reached");
        Expect(oldOld != 0, "real_old_old_control_reached");
        Expect(oldYoung + oldYoungFast != 0, "real_old_young_field_reached");
    }
    std::printf("P2_FAST old_young=%u finalizable_young=%u\n", oldYoungFast, finalYoungFast);
    std::printf("P2_RESULT failures=%u remset=%u follow=%u old_old=%u old_young=%u\n",
                failures.load(), remsetChild, youngFollow, oldOld, oldYoung);
    heap.RemoveExportObject(root);
    if (minorOnly) heap.RemoveExportObject(controlRoot);
    return failures.load();
}

// Stop at a product breakpoint after the roots task has consumed its storage
// snapshot. These are real mutator allocations/registrations, never mark-state
// injection or a substitute for a GC producer.
extern "C" int p2FinalizerRegistrationExercise()
{
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* smallType = Type(types[0], false, 1);
    auto* largeType = Type(types[1], false, 32768);
    smallType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    largeType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    auto* delayedOld = MObject::NewPinnedObject(smallType, 16);
    NativeSlot oldRoot(zpointer::null);
    Heap::GetBarrier().WriteStaticRef(oldRoot, delayedOld);
    NativeSlot* roots[] = { &oldRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    const size_t discovered = references.Discovered(ReferenceType::FINAL);
    const size_t enqueued = references.Enqueued(ReferenceType::FINAL);
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "late_registration_product_breakpoint");
    auto* small = MObject::NewObject(smallType, 16, AllocType::MOVEABLE_OBJECT);
    auto* large = MObject::NewObject(largeType, largeType->GetInstanceSize() + sizeof(uintptr_t),
                                     AllocType::MOVEABLE_OBJECT);
    auto* pinned = MObject::NewPinnedObject(smallType, 16);
    small->OnFinalizerCreated();
    large->OnFinalizerCreated();
    pinned->OnFinalizerCreated();
    delayedOld->OnFinalizerCreated();
    Expect(RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(small))->IsAllocating(), "late_small_allocating");
    Expect(RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(large))->IsAllocating(), "late_large_allocating");
    Expect(RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(pinned))->IsAllocating(), "late_pinned_allocating");
    auto* oldPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(delayedOld));
    Expect(!oldPage->IsAllocating(), "late_old_registration_is_not_new_allocation");
    auto dumpRegistrations = [&](const char* stage) {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            std::printf("P2_REGISTERED stage=%s slot=%p word=%#zx object=%p young=%d birth=%llu epoch=%llu\n",
                        stage, &slot, raw(word), object, page->IsYoungRegion(),
                        static_cast<unsigned long long>(page->BirthSequence()),
                        static_cast<unsigned long long>(page->GetSnapshotEpoch()));
        });
    };
    dumpRegistrations("after-register");
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Expect(oldPage->IsMarkedObject(oldPage->GetMarkView<Generation::Old>(), delayedOld), "late_old_retained_by_original_strong_root");
    Expect(references.Discovered(ReferenceType::FINAL) == discovered, "late_registrations_not_rediscovered_after_mark");
    Expect(references.Enqueued(ReferenceType::FINAL) == enqueued, "late_registrations_not_enqueued_in_first_cycle");
    dumpRegistrations("first-complete");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    bool youngSmall = false, youngLarge = false;
    size_t oldCandidates = 0;
    bool pinnedDiscovered = false, delayedDiscovered = false;
    TracingCollector::testYoungMarkCompleted = [&] {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            if (!page->IsYoungRegion()) return;
            if (object->GetTypeInfo() == smallType) {
                youngSmall = true;
                Expect(page->IsMarkedObject(page->GetMarkView<Generation::Young>(), object), "late_small_next_young_marked");
            } else if (object->GetTypeInfo() == largeType) {
                youngLarge = true;
                Expect(page->IsMarkedObject(page->GetMarkView<Generation::Young>(), object), "late_large_next_young_marked");
            }
        });
    };
    TracingCollector::testOldMarkStarted = [&] {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            if (page->IsYoungRegion()) return;
            ++oldCandidates;
            Expect(page->IsResurrectedObject(object), "late_old_candidate_discovered_during_mark");
            if (object == pinned) pinnedDiscovered = page->IsResurrectedObject(object);
            if (object == delayedOld) delayedDiscovered = page->IsResurrectedObject(object);
        });
    };
    collector.RequestGC(GC_REASON_USER, false);
    TracingCollector::testYoungMarkCompleted = nullptr;
    TracingCollector::testOldMarkStarted = nullptr;
    dumpRegistrations("next-complete");
    Expect(youngSmall, "late_small_next_young_consumer_reached");
    Expect(youngLarge, "late_large_next_young_consumer_reached");
    Expect(pinnedDiscovered, "late_pinned_next_old_discovered");
    Expect(delayedDiscovered, "late_existing_old_next_old_discovered");
    Expect(references.Discovered(ReferenceType::FINAL) - discovered == oldCandidates, "late_old_registrations_discovered_next_cycle");
    Expect(references.Enqueued(ReferenceType::FINAL) - enqueued == oldCandidates, "late_old_registrations_enqueued_once_next_cycle");
    std::printf("P2_REGISTRATION_RESULT failures=%u discovered=%zu enqueued=%zu\n", failures.load(),
                references.Discovered(ReferenceType::FINAL) - discovered,
                references.Enqueued(ReferenceType::FINAL) - enqueued);
    return failures.load();
}

// Inspect finalizable closure output before processing/reclamation, so a
// severed producer reports liveness invariants instead of failing on a later
// attempt to remap a target that the faulty product did not retain.
extern "C" int p2FinalizerClosureExercise()
{
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* edgeType = Type(types[0], true, 1);
    auto* leafType = Type(types[1], false, 1);
    edgeType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto& heap = Heap::GetHeap();
    auto& barrier = Heap::GetBarrier();
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    auto* holder = MObject::NewPinnedObject(edgeType, 16);
    auto* child = MObject::NewPinnedObject(edgeType, 16);
    auto* sentinel = MObject::NewPinnedObject(leafType, 16);
    auto* upgraded = MObject::NewPinnedObject(edgeType, 16);
    auto* control = MObject::NewPinnedObject(leafType, 16);
    barrier.WriteReference(holder, Slot(holder), child);
    barrier.WriteReference(child, Slot(child), sentinel);
    barrier.WriteReference(upgraded, Slot(upgraded), control);
    holder->OnFinalizerCreated();
    upgraded->OnFinalizerCreated();
    NativeSlot strongRoot(zpointer::null);
    barrier.WriteStaticRef(strongRoot, upgraded);
    NativeSlot* roots[] = { &strongRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    const size_t discovered = references.Discovered(ReferenceType::FINAL);
    const size_t enqueued = references.Enqueued(ReferenceType::FINAL);
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "finalizable_product_mark_end_breakpoint");
    auto isFinal = [](BaseObject* object) {
        return RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object))->IsResurrectedObject(object);
    };
    auto isStrong = [](BaseObject* object) {
        auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        return page->IsMarkedObject(page->GetMarkView<Generation::Old>(), object);
    };
    Expect(isFinal(holder), "finalizable_registered_holder_live");
    Expect(isFinal(child), "finalizable_field_child_live");
    Expect(isFinal(sentinel), "finalizable_field_sentinel_live");
    Expect(!isStrong(child), "finalizable_field_child_not_strong");
    Expect(ZPointer::is_marked_finalizable(Slot(holder).GetFieldValue()), "finalizable_field_preserves_final_color");
    Expect(isStrong(upgraded) && !isFinal(upgraded), "finalizable_root_strong_upgrade");
    Expect(isStrong(control), "finalizable_independent_strong_follow_control");
    Expect(references.Discovered(ReferenceType::FINAL) - discovered == 2, "finalizable_mark_discovery_once");
    if (failures.load() != 0) {
        std::printf("P2_CLOSURE_RESULT failures=%u target_stage=mark_end\n", failures.load());
        std::fflush(stdout);
        std::_Exit(failures.load());
    }
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Expect(references.Enqueued(ReferenceType::FINAL) - enqueued == 1, "finalizable_processing_excludes_strong_upgrade");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_CLOSURE_RESULT failures=%u target_stage=complete\n", failures.load());
    return failures.load();
}

extern "C" int p2ArrayFieldExercise()
{
    alignas(TypeInfo) static unsigned char types[4][sizeof(TypeInfo)]{};
    auto* leafType = Type(types[0], false, 1);
    auto* holderType = Type(types[1], true, 1);
    holderType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto* arrayType = reinterpret_cast<TypeInfo*>(types[2]);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    const bool structArray = std::getenv("P2_STRUCT_ARRAY") != nullptr;
    auto* structType = Type(types[3], true, 2);
    structType->SetType(TypeKind::TYPE_KIND_STRUCT);
    arrayType->SetComponentTypeInfo(structArray ? structType : leafType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<MAddress>(arrayType), sizeof(TypeInfo));
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto& barrier = Heap::GetBarrier();
    const bool finalizable = std::getenv("P2_ARRAY_FINALIZABLE") != nullptr;
    auto* first = MObject::NewPinnedObject(leafType, 16);
    auto* last = MObject::NewPinnedObject(leafType, 16);
    auto* control = MObject::NewPinnedObject(leafType, 16);
    auto* holder = MObject::NewPinnedObject(holderType, 16);
    barrier.WriteReference(holder, Slot(holder), control);
    if (finalizable) holder->OnFinalizerCreated();
    collector.RequestGC(GC_REASON_YOUNG, false);
    const size_t length = 2 * MarkPartialArray::MIN_LENGTH + 17;
    const size_t fieldCount = length * (structArray ? 2 : 1);
    MArray* array = structArray ? MCC_NewArray(arrayType, length) : MCC_NewObjArray(arrayType, length);
    auto* elements = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
    for (size_t index = 0; index < fieldCount; ++index) {
        barrier.WriteReference(array, elements[index], index % 2 == 0 ? first : last);
    }
    barrier.WriteReference(holder, Slot(holder), array);
    NativeSlot arrayRoot(zpointer::null), controlRoot(zpointer::null);
    barrier.WriteStaticRef(arrayRoot, array);
    barrier.WriteStaticRef(controlRoot, control);
    NativeSlot* roots[] = { &controlRoot, &arrayRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), finalizable ? 1 : 2);
    std::atomic<size_t> fields{0};
    std::atomic<bool> rangeTarget{false};
    auto arrayAddress = [&] {
        return raw(finalizable ? Slot(holder).GetTargetObject() : arrayRoot.GetTargetObject());
    };
    Barrier::testFieldMarkResult = [&](Barrier::FieldMarkKind kind, RefField<>& field, zpointer, zaddress) {
        const auto expected = finalizable ? Barrier::FieldMarkKind::Finalizable : Barrier::FieldMarkKind::Old;
        if (kind != expected) return;
        auto* current = reinterpret_cast<MArray*>(arrayAddress());
        const MAddress begin = reinterpret_cast<MAddress>(current->ConvertToCArray());
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (address >= begin && address < begin + fieldCount * sizeof(uintptr_t)) {
            ++fields;
            if (address == begin + (fieldCount - 1) * sizeof(uintptr_t)) rangeTarget = true;
        }
    };
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "array_product_mark_end_breakpoint");
    auto* current = reinterpret_cast<MArray*>(arrayAddress());
    auto* firstPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(first));
    auto* lastPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(last));
    auto* controlPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(control));
    Expect(!RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(current))->IsYoungRegion(), "array_real_old_owner");
    Expect(fields.load() == fieldCount, structArray ? "struct_array_visits_all_members" : "array_full_and_range_visit_all_fields");
    Expect(rangeTarget.load(), structArray ? "struct_array_last_member_reached" : "array_range_target_reached");
    Expect(finalizable ? firstPage->IsResurrectedObject(first) :
        firstPage->IsMarkedObject(firstPage->GetMarkView<Generation::Old>(), first), "array_first_child_retained_in_domain");
    Expect(finalizable ? lastPage->IsResurrectedObject(last) :
        lastPage->IsMarkedObject(lastPage->GetMarkView<Generation::Old>(), last), "array_last_child_retained_in_domain");
    Expect(controlPage->IsMarkedObject(controlPage->GetMarkView<Generation::Old>(), control), "array_independent_strong_control");
    Barrier::testFieldMarkResult = nullptr;
    std::printf("P2_ARRAY_RESULT failures=%u finalizable=%d fields=%zu expected=%zu range_target=%d\n",
                failures.load(), finalizable, fields.load(), fieldCount, rangeTarget.load());
    std::fflush(stdout);
    if (failures.load() != 0) std::_Exit(failures.load());
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), finalizable ? 1 : 2);
    return failures.load();
}

namespace {
class P2FieldInputTask final : public GCWorkerTask {
public:
    P2FieldInputTask(WCollector& collector, std::function<void()> exercise)
        : collector(collector), exercise(std::move(exercise)) {}
    void Work(uint32_t) override
    {
        if (!claimed.exchange(true)) exercise();
        // Same GC-worker tail protocol as MarkOldRootsTask. The field entry
        // produced these entries; the test never supplies mark/current output.
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *collector.MajorMarkDomain());
        Expect(collector.MajorMarkDomain()->Stacks().IsEmpty(), "slow_input_worker_tls_drained");
    }
private:
    WCollector& collector;
    std::function<void()> exercise;
    std::atomic<bool> claimed{false};
};
}

// Advisor 175006Z: field API input layer. The existing young-mark-start
// observation pauses after real old mark-start and before remset scanning.
// Every word is from a real store; no masks, phases or mark results are seeded.
extern "C" int p2SlowFieldInputExercise()
{
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)]{};
    auto* holderType = Type(types[0], true, 2);
    holderType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto* edgeType = Type(types[1], true, 1);
    auto* leafType = Type(types[2], false, 1);
    auto& heap = Heap::GetHeap();
    auto& collector = static_cast<WCollector&>(heap.GetCollector());
    auto& barrier = Heap::GetBarrier();
    auto* strongHolder = MObject::NewPinnedObject(holderType, 24);
    auto* finalHolder = MObject::NewPinnedObject(holderType, 24);
    auto* oldChild = MObject::NewPinnedObject(edgeType, 16);
    auto* oldSentinel = MObject::NewPinnedObject(leafType, 16);
    auto* finalChild = MObject::NewPinnedObject(edgeType, 16);
    auto* finalSentinel = MObject::NewPinnedObject(leafType, 16);
    barrier.WriteReference(oldChild, Slot(oldChild), oldSentinel);
    barrier.WriteReference(finalChild, Slot(finalChild), finalSentinel);
    barrier.WriteReference(strongHolder, Slot(strongHolder), oldChild);
    barrier.WriteReference(strongHolder, Slot(strongHolder, 1), oldChild);
    barrier.WriteReference(finalHolder, Slot(finalHolder), finalChild);
    barrier.WriteReference(finalHolder, Slot(finalHolder, 1), finalChild);
    finalHolder->OnFinalizerCreated();
    NativeSlot strongRoot(zpointer::null);
    barrier.WriteStaticRef(strongRoot, strongHolder);
    NativeSlot* roots[] = { &strongRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    collector.RequestGC(GC_REASON_YOUNG, false);
    auto* young = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    auto* youngSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    barrier.WriteReference(young, Slot(young), youngSentinel);
    barrier.WriteReference(strongHolder, Slot(strongHolder), young);
    barrier.WriteReference(finalHolder, Slot(finalHolder), young);
    const zpointer stored = Slot(strongHolder).GetFieldValue();
    std::atomic<unsigned> strongSlow{0}, finalSlow{0}, strongFast{0}, finalFast{0};
    std::atomic<unsigned> strongFollow{0}, finalFollow{0};
    std::atomic<bool> inputTask{false};
    Barrier::testFieldMarkResult = [&](Barrier::FieldMarkKind kind, RefField<>& field, zpointer observed, zaddress result) {
        if (inputTask.load()) {
            if (&field == &Slot(strongHolder)) {
                ++strongSlow;
                Expect(!ZPointer::is_mark_good(observed), "strong_young_slow_input_selected");
                Expect(is_null(result), "strong_young_slow_no_object_result");
                Expect(field.GetFieldValue() == observed, "strong_young_slow_does_not_heal");
            } else if (&field == &Slot(finalHolder)) {
                ++finalSlow;
                Expect(!(ZPointer::is_load_good(observed) && ZPointer::is_marked_any_old(observed)), "final_young_slow_input_selected");
                Expect(is_null(result), "final_young_slow_no_object_result");
                Expect(field.GetFieldValue() == observed, "final_young_slow_does_not_heal");
            } else if (&field == &Slot(strongHolder, 1)) {
                Expect(to_object(result) == oldChild, "strong_old_slow_current_control");
                if (ZPointer::is_mark_good(observed)) {
                    ++strongFast;
                    Expect(field.GetFieldValue() == observed, "strong_old_fast_unchanged");
                }
            } else if (&field == &Slot(finalHolder, 1)) {
                Expect(to_object(result) == finalChild, "final_old_slow_current_control");
                if (ZPointer::is_load_good(observed) && ZPointer::is_marked_any_old(observed)) {
                    ++finalFast;
                    Expect(field.GetFieldValue() == observed, "final_old_fast_unchanged");
                }
            }
        }
        if (&field == &Slot(oldChild) && kind == Barrier::FieldMarkKind::Old) ++strongFollow;
        if (&field == &Slot(finalChild) && kind == Barrier::FieldMarkKind::Finalizable) ++finalFollow;
    };
    unsigned started = 0;
    TracingCollector::testYoungMarkStarted = [&] {
        if (!collector.GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) return;
        ++started;
        Expect(Slot(strongHolder).GetFieldValue() == stored, "slow_input_original_store_word_preserved");
        auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(young));
        auto bit = [&] {
            auto* bitmap = page->GetMarkBitmap(page->GetMarkView<Generation::Young>());
            return bitmap != nullptr && bitmap->IsMarked(page->GetAddressOffset(reinterpret_cast<MAddress>(young)));
        };
        const bool before = bit();
        P2FieldInputTask task(collector, [&] {
            auto& youngStacks = collector.YoungMarkDomain()->Stacks();
            const size_t youngBefore = youngStacks.Population();
            const size_t oldBefore = collector.MajorMarkDomain()->Stacks().Population();
            TracingCollector::WorkStack work;
            inputTask = true;
            collector.TraceRefField(strongHolder, Slot(strongHolder), work, false);
            collector.TraceRefField(finalHolder, Slot(finalHolder), work, true);
            collector.TraceRefField(strongHolder, Slot(strongHolder, 1), work, false);
            collector.TraceRefField(finalHolder, Slot(finalHolder, 1), work, true);
            collector.TraceRefField(strongHolder, Slot(strongHolder, 1), work, false);
            collector.TraceRefField(finalHolder, Slot(finalHolder, 1), work, true);
            inputTask = false;
            Expect(youngStacks.Population() == youngBefore, "slow_old_fields_do_not_publish_young_entries");
            Expect(collector.MajorMarkDomain()->Stacks().Population() > oldBefore, "slow_old_controls_publish_real_entries");
        });
        collector.GetGenerationCycle(GCCycleGeneration::OLD).Workers()->Run(task);
        Expect(bit() == before, "slow_old_fields_do_not_write_young_bitmap");
        Expect(strongSlow == 1 && finalSlow == 1, "slow_input_both_field_entries_reached");
        Expect(strongFast == 1 && finalFast == 1, "slow_input_legal_fast_controls_reached");
        if (failures.load() != 0) {
            std::printf("P2_SLOW_RESULT failures=%u target_stage=field_result\n", failures.load());
            std::fflush(stdout);
            std::_Exit(failures.load());
        }
    };
    collector.RequestGC(GC_REASON_HEU_SYNC, false);
    TracingCollector::testYoungMarkStarted = nullptr;
    Barrier::testFieldMarkResult = nullptr;
    Expect(started == 1, "slow_input_real_major_roots_phase_reached");
    auto* strongPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(oldSentinel));
    auto* finalPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(finalSentinel));
    Expect(strongFollow != 0 && strongPage->IsMarkedObject(strongPage->GetMarkView<Generation::Old>(), oldSentinel),
           "slow_old_strong_control_followed_by_product");
    Expect(finalFollow != 0 && finalPage->IsResurrectedObject(finalSentinel),
           "slow_old_final_control_followed_by_product");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_SLOW_RESULT failures=%u strong=%u final=%u strong_follow=%u final_follow=%u\n",
                failures.load(), strongSlow.load(), finalSlow.load(), strongFollow.load(), finalFollow.load());
    return failures.load();
}
