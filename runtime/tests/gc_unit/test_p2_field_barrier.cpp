// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "ObjectModel/MArray.h"
#include "Heap/z/zMarkPartialArray.hpp"
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
bool IsFinalizable(BaseObject* object)
{
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    return page->is_live_bit_set(from_object(object)) && !page->is_strong_bit_set(from_object(object));
}
RefField<>& Slot(BaseObject* object, unsigned index = 0)
{
    return HeapSlotAt<>(reinterpret_cast<MAddress>(object) + sizeof(uintptr_t) * (index + 1));
}

}

// Isolate the promotion invariant from subsequent field-barrier scenarios so
// a failed promotion cannot hide the target behind a later phase assertion.
extern "C" int p2PinnedPromotionExercise()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = Type(storage, false, 1);
    auto& heap = Heap::GetHeap();
    BaseObject* pinned = MObject::NewPinnedObject(type, 16);
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, pinned);
    NativeSlot* roots[] = { &root };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    Expect(Heap::page(reinterpret_cast<MAddress>(pinned))->IsYoungRegion(), "real_pinned_holder_starts_young");
    heap.RequestGC(GC_REASON_USER, false);
    BaseObject* holder = ZBarrier::ReadStaticRef(root);
    Expect(!Heap::page(reinterpret_cast<MAddress>(holder))->IsYoungRegion(), "real_holder_is_old");
    Expect(holder == pinned, "real_pinned_holder_address_unchanged");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_PINNED_RESULT failures=%u\n", failures.load());
    return failures.load();
}

extern "C" int p2RawPointerPromotionExercise()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = Type(storage, false, 1);
    auto& heap = Heap::GetHeap();
    BaseObject* object = MObject::NewObject(type, 16, AllocType::MOVEABLE_OBJECT);
    object = heap.PinRawPointerObject(object);
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, object);
    NativeSlot* roots[] = { &root };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    auto* before = Heap::page(reinterpret_cast<MAddress>(object));
    const int32_t count = before->GetRawPointerObjectCount();
    Expect(before->IsYoungRegion() && count > 0, "real_raw_pointer_starts_young_and_pinned");
    heap.RequestGC(GC_REASON_USER, false);
    auto* current = ZBarrier::ReadStaticRef(root);
    auto* page = Heap::page(reinterpret_cast<MAddress>(current));
    Expect(!page->IsYoungRegion(), "real_raw_pointer_is_old");
    Expect(current == object, "real_raw_pointer_address_unchanged");
    Expect(page->GetRawPointerObjectCount() == count, "real_raw_pointer_count_survives_promotion");
    // Observe the count invariant before Release can reject invalid metadata.
    if (page->GetRawPointerObjectCount() > 0) {
        heap.RemoveRawPointerObject(current);
        Expect(page->GetRawPointerObjectCount() == count - 1, "real_raw_pointer_release_consumes_count");
    }
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_RAW_POINTER_RESULT failures=%u\n", failures.load());
    return failures.load();
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
    auto& collector = heap;
    BaseObject* holder = MObject::NewPinnedObject(holderType, 24);
    BaseObject* const pinnedHolder = holder;
    BaseObject* oldChild = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    NativeSlot holderRoot(zpointer::null);
    ZBarrier::WriteStaticRef(holderRoot, holder);
    NativeSlot* holderRoots[] = { &holderRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(holderRoots), 1);
    ZBarrier::WriteReference(holder, Slot(holder), oldChild);
    ZBarrier::WriteReference(holder, Slot(holder, 1), oldChild);
    const bool finalizableCase = std::getenv("P2_FINALIZABLE") != nullptr;
    BaseObject* finalHolder = nullptr;
    BaseObject* finalOld = nullptr;
    BaseObject* upgraded = nullptr;
    NativeSlot upgradeRoot(zpointer::null);
    NativeSlot* upgradeRoots[] = { &upgradeRoot };
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    const auto discoveredBefore = references.Discovered(ReferenceType::FINAL);
    const auto enqueuedBefore = references.Enqueued(ReferenceType::FINAL);
    if (finalizableCase) {
        finalHolder = MObject::NewObject(finalType, 24, AllocType::MOVEABLE_OBJECT);
        finalOld = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
        auto* finalSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
        ZBarrier::WriteReference(finalOld, Slot(finalOld), finalSentinel);
        ZBarrier::WriteReference(finalHolder, Slot(finalHolder), oldChild);
        ZBarrier::WriteReference(finalHolder, Slot(finalHolder, 1), finalOld);
        upgraded = MObject::NewObject(finalType, 24, AllocType::MOVEABLE_OBJECT);
        ZBarrier::WriteReference(upgraded, Slot(upgraded), oldChild);
        ZBarrier::WriteStaticRef(upgradeRoot, upgraded);
        heap.RegisterStaticRoots(reinterpret_cast<Uptr>(upgradeRoots), 1);
    }
    // Full GC's preclean promotes these genuinely rooted objects through
    // the product selector/flip/remset path (ZGC zDriver.cpp:416-436).
    // Register finalizers only after this setup cycle, to avoid classifying them
    // before the field scenario starts.
    BaseObject* oldViaYoung = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    NativeSlot oldViaRoot(zpointer::null);
    ZBarrier::WriteStaticRef(oldViaRoot, oldViaYoung);
    NativeSlot* oldViaRoots[] = { &oldViaRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(oldViaRoots), 1);
    NativeSlot finalSetupRoot(zpointer::null);
    NativeSlot* setupRoots[] = { &finalSetupRoot };
    if (finalizableCase) {
        ZBarrier::WriteStaticRef(finalSetupRoot, finalHolder);
        heap.RegisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    }
    heap.RequestGC(GC_REASON_USER, false);
    holder = ZBarrier::ReadStaticRef(holderRoot);
    oldChild = ZBarrier::ReadReference(holder, Slot(holder, 1));
    oldViaYoung = ZBarrier::ReadStaticRef(oldViaRoot);
    if (finalizableCase) {
        finalHolder = ZBarrier::ReadStaticRef(finalSetupRoot);
        finalOld = ZBarrier::ReadReference(finalHolder, Slot(finalHolder, 1));
        upgraded = ZBarrier::ReadStaticRef(upgradeRoot);
    }
    if (finalizableCase) {
        heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
        finalHolder->OnFinalizerCreated();
        upgraded->OnFinalizerCreated();
    }

    // Advance a real young epoch before overwriting the old slot. Its previous
    // non-null word must go through the store barrier and remember the slot.
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    holder = ZBarrier::ReadStaticRef(holderRoot);
    oldChild = ZBarrier::ReadReference(holder, Slot(holder, 1));
    oldViaYoung = ZBarrier::ReadStaticRef(oldViaRoot);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(oldViaRoots), 1);
    auto* child = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
    auto* sentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(child, Slot(child), sentinel);
    ZBarrier::WriteReference(child, Slot(child, 1), oldViaYoung);
    ZBarrier::WriteReference(holder, Slot(holder), child);
    if (finalizableCase) ZBarrier::WriteReference(finalHolder, Slot(finalHolder), child);
    auto* rootedControl = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    const U64 controlRoot = heap.RegisterExportRoot(rootedControl);
    Expect(!Heap::page(reinterpret_cast<MAddress>(holder))->IsYoungRegion(), "real_holder_is_old");
    Expect(holder == pinnedHolder, "real_pinned_holder_address_unchanged");
    Expect(Heap::page(reinterpret_cast<MAddress>(child))->IsYoungRegion(), "real_child_is_young");
    unsigned finalOldOld = 0, finalOldYoung = 0, finalFollow = 0, finalYoungFast = 0;
    unsigned remsetChild = 0, oldOld = 0, oldYoung = 0, youngFollow = 0, oldYoungFast = 0;
    unsigned youngOldMajor = 0, youngOldMinor = 0;
    // A minor cycle preserves old mark bits from the preceding major. Compare
    // with the actual pre-cycle value, not an assumed zero (ZGC zBarrier.cpp:158-183).
    const bool oldBeforeMinor = Heap::page(reinterpret_cast<MAddress>(oldViaYoung))->is_strong_bit_set(from_object(oldViaYoung));
    BaseObject* currentChild = child;
    ZBarrier::testFieldMarkResult = [&](ZBarrier::FieldMarkKind kind, RefField<>& field,
                                      zpointer observed, zaddress result) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (kind == ZBarrier::FieldMarkKind::Finalizable && finalHolder != nullptr) {
            Expect((Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old).phase == ZGenerationPhase::Mark ||
                    Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old).phase == ZGenerationPhase::MarkComplete), "finalizable_follow_during_mark");
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
                auto* page = Heap::page(raw(result));
                Expect(IsFinalizable(finalOld), "finalizable_old_is_finalizable");
                Expect(!page->is_strong_bit_set(from_object(finalOld)), "finalizable_old_not_strong");
                Expect(ZPointer::is_marked_finalizable(field.GetFieldValue()), "finalizable_old_slot_color");
            }
            if (&field == &Slot(finalOld)) {
                ++finalFollow;
                Expect(!is_null(result), "finalizable_old_child_followed");
            }
        }
        if (&field == &Slot(holder) && kind == ZBarrier::FieldMarkKind::Remset) {
            ++remsetChild;
            currentChild = to_object(result);
            Expect(currentChild != nullptr, "remset_returns_child");
            if (currentChild != nullptr) {
                auto* page = Heap::page(raw(result));
                if (page->IsYoungRegion()) {
                    Expect(page->is_strong_bit_set(from_object(currentChild)), "remset_child_marked");
                } else {
                    Expect(ZPointer::is_marked_young(field.GetFieldValue()), "remset_old_target_young_good");
                }
            }
            Expect(ZPointer::is_load_good(field.GetFieldValue()) &&
                   ZPointer::is_marked_young(field.GetFieldValue()), "remset_slot_young_good");
        }
        if (&field == &Slot(holder, 1) && kind == ZBarrier::FieldMarkKind::Old) {
            ++oldOld;
            Expect(to_object(result) == oldChild, "old_old_returns_current");
            auto* page = Heap::page(reinterpret_cast<MAddress>(oldChild));
            Expect(page->is_strong_bit_set(from_object(oldChild)), "old_old_marked");
        }
        if (&field == &Slot(holder) && kind == ZBarrier::FieldMarkKind::Old) {
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
        if (currentChild != nullptr && &field == &Slot(currentChild, 1) && kind == ZBarrier::FieldMarkKind::Young) {
            const bool major = Heap::GetHeap().GetZGeneration(ZGenerationId::young).IsMajorRoots();
            if (major) ++youngOldMajor; else ++youngOldMinor;
            Expect(to_object(result) == oldViaYoung, "young_old_returns_current");
            auto* page = Heap::page(reinterpret_cast<MAddress>(oldViaYoung));
            const bool marked = page->is_strong_bit_set(from_object(oldViaYoung));
            Expect(major ? marked : marked == oldBeforeMinor,
                   major ? "young_old_major_marks" : "young_old_minor_preserves_old_mark");
            Expect(ZPointer::is_store_good(field.GetFieldValue()), "young_old_heals_store_good");
        }
        if (currentChild != nullptr && &field == &Slot(currentChild) && kind == ZBarrier::FieldMarkKind::Young) {
            ++youngFollow;
            Expect(!is_null(result), "young_child_follows_sentinel");
            Expect(!ZPointer::is_store_bad(field.GetFieldValue()), "young_field_store_good");
        }
    };
    const bool minorOnly = std::getenv("P2_MINOR_ONLY") != nullptr;
    // Read the actual bitmap before relocation can make an unmarked target
    // unavailable. These are the target invariants, not existence guards.
    ZGeneration::testYoungMarkCompleted = [&] {
        auto* childPage = Heap::page(reinterpret_cast<MAddress>(child));
        auto* sentinelPage = Heap::page(reinterpret_cast<MAddress>(sentinel));
        auto* controlPage = Heap::page(reinterpret_cast<MAddress>(rootedControl));
        Expect(childPage->is_strong_bit_set(from_object(child)),
               "remset_retains_child_first_cycle");
        Expect(sentinelPage->is_strong_bit_set(from_object(sentinel)),
               "remset_retains_sentinel_first_cycle");
        Expect(controlPage->is_strong_bit_set(from_object(rootedControl)),
               "independent_young_root_control");
    };
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    ZGeneration::testYoungMarkCompleted = nullptr;
    if (failures.load() != 0) {
        // The target state has been observed. Do not dereference an object
        // that the faulty product has just classified as unreachable.
        ZBarrier::testFieldMarkResult = nullptr;
        heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(holderRoots), 1);
        heap.RemoveExportObject(controlRoot);
        std::printf("P2_FAST old_young=%u finalizable_young=%u\n", oldYoungFast, finalYoungFast);
        std::printf("P2_RESULT failures=%u target_stage=first_cycle\n", failures.load());
        return failures.load();
    }
    // A second cycle removes the store-buffer current-value marking side path:
    // the unchanged edge must now survive through the remembered slot itself.
    BaseObject* childBeforeNext = ZBarrier::ReadReference(holder, Slot(holder));
    currentChild = childBeforeNext;
    BaseObject* sentinelBeforeNext = ZBarrier::ReadReference(childBeforeNext, Slot(childBeforeNext));
    unsigned completed = 0;
    ZGeneration::testYoungMarkCompleted = [&] {
        ++completed;
        auto* childPage = Heap::page(reinterpret_cast<MAddress>(childBeforeNext));
        auto* sentinelPage = Heap::page(reinterpret_cast<MAddress>(sentinelBeforeNext));
        Expect(childPage->is_strong_bit_set(from_object(childBeforeNext)),
               "remset_retains_child_next_cycle");
        Expect(sentinelPage->is_strong_bit_set(from_object(sentinelBeforeNext)),
               "remset_retains_sentinel_next_cycle");
    };
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    ZGeneration::testYoungMarkCompleted = nullptr;
    Expect(completed != 0, "young_completion_observation_reached");
    if (!minorOnly) {
        // The control was needed only for remset liveness. End its real export
        // lifetime before the independent old-field scenario.
        heap.RemoveExportObject(controlRoot);
        currentChild = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
        auto* freshSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
        ZBarrier::WriteReference(currentChild, Slot(currentChild), freshSentinel);
        ZBarrier::WriteReference(currentChild, Slot(currentChild, 1), oldViaYoung);
        ZBarrier::WriteReference(holder, Slot(holder), currentChild);
        if (finalizableCase) ZBarrier::WriteReference(finalHolder, Slot(finalHolder), currentChild);
        Heap::GetHeap().RequestGC(GC_REASON_HEU_SYNC, false);
    }
    ZBarrier::testFieldMarkResult = nullptr;
    if (finalizableCase && !minorOnly) {
        Expect(finalOldOld != 0, "real_finalizable_old_control_reached");
        Expect(finalOldYoung + finalYoungFast != 0, "real_finalizable_young_field_reached");
        Expect(finalFollow != 0, "real_finalizable_follow_reached");
        std::printf("P2_FINALIZABLE discovered_delta=%llu enqueued_delta=%llu\n",
                    static_cast<unsigned long long>(references.Discovered(ReferenceType::FINAL) - discoveredBefore),
                    static_cast<unsigned long long>(references.Enqueued(ReferenceType::FINAL) - enqueuedBefore));
        Expect(references.Discovered(ReferenceType::FINAL) - discoveredBefore == 2, "finalizable_discovered_once_each");
        Expect(references.Enqueued(ReferenceType::FINAL) - enqueuedBefore == 1, "finalizable_only_unupgraded_enqueued");
        auto* page = Heap::page(reinterpret_cast<MAddress>(upgraded));
        Expect(page->is_strong_bit_set(from_object(upgraded)), "finalizable_upgraded_to_strong");
        Expect(!IsFinalizable(upgraded), "finalizable_upgraded_not_pending");
        heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(upgradeRoots), 1);
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
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(holderRoots), 1);
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
    auto& collector = heap;
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    BaseObject* delayedOld = MObject::NewObject(smallType, 16, AllocType::MOVEABLE_OBJECT);
    NativeSlot oldRoot(zpointer::null);
    ZBarrier::WriteStaticRef(oldRoot, delayedOld);
    NativeSlot* roots[] = { &oldRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    const size_t discovered = references.Discovered(ReferenceType::FINAL);
    const size_t enqueued = references.Enqueued(ReferenceType::FINAL);
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "late_registration_product_breakpoint");
    delayedOld = ZBarrier::ReadStaticRef(oldRoot);
    auto* small = MObject::NewObject(smallType, 16, AllocType::MOVEABLE_OBJECT);
    auto* large = MObject::NewObject(largeType, largeType->GetInstanceSize() + sizeof(uintptr_t),
                                     AllocType::MOVEABLE_OBJECT);
    auto* pinned = MObject::NewPinnedObject(smallType, 16);
    small->OnFinalizerCreated();
    large->OnFinalizerCreated();
    pinned->OnFinalizerCreated();
    delayedOld->OnFinalizerCreated();
    Expect(Heap::page(reinterpret_cast<MAddress>(small))->IsAllocating(), "late_small_allocating");
    Expect(Heap::page(reinterpret_cast<MAddress>(large))->IsAllocating(), "late_large_allocating");
    Expect(Heap::page(reinterpret_cast<MAddress>(pinned))->IsAllocating(), "late_pinned_allocating");
    auto* oldPage = Heap::page(reinterpret_cast<MAddress>(delayedOld));
    Expect(!oldPage->IsAllocating(), "late_old_registration_is_not_new_allocation");
    auto dumpRegistrations = [&](const char* stage) {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = Heap::page(reinterpret_cast<MAddress>(object));
            std::printf("P2_REGISTERED stage=%s slot=%p word=%#zx object=%p young=%d birth=%llu epoch=%llu\n",
                        stage, &slot, raw(word), object, page->IsYoungRegion(),
                        static_cast<unsigned long long>(page->BirthSequence()),
                        static_cast<unsigned long long>(page->GetSnapshotEpoch()));
        });
    };
    dumpRegistrations("after-register");
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    Expect(oldPage->is_strong_bit_set(from_object(delayedOld)), "late_old_retained_by_original_strong_root");
    Expect(references.Discovered(ReferenceType::FINAL) == discovered, "late_registrations_not_rediscovered_after_mark");
    Expect(references.Enqueued(ReferenceType::FINAL) == enqueued, "late_registrations_not_enqueued_in_first_cycle");
    dumpRegistrations("first-complete");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    bool youngSmall = false, youngLarge = false;
    size_t oldCandidates = 0;
    bool pinnedDiscovered = false, delayedDiscovered = false;
    ZGeneration::testYoungMarkCompleted = [&] {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = Heap::page(reinterpret_cast<MAddress>(object));
            if (!page->IsYoungRegion()) return;
            if (object->GetTypeInfo() == smallType) {
                youngSmall = true;
                Expect(page->is_strong_bit_set(from_object(object)), "late_small_next_young_marked");
            } else if (object->GetTypeInfo() == largeType) {
                youngLarge = true;
                Expect(page->is_strong_bit_set(from_object(object)), "late_large_next_young_marked");
            }
        });
    };
    ZGeneration::testOldMarkStarted = [&] {
        heap.GetFinalizerProcessor().VisitFinalizers([&](NativeSlot& slot) {
            const auto word = slot.GetFieldValue();
            if (is_null_any(word)) return;
            auto* object = to_object(RefField<>(word).GetTargetObject());
            auto* page = Heap::page(reinterpret_cast<MAddress>(object));
            if (page->IsYoungRegion()) return;
            ++oldCandidates;
            Expect(IsFinalizable(object), "late_old_candidate_discovered_during_mark");
            if (object == pinned) pinnedDiscovered = IsFinalizable(object);
            if (object == delayedOld) delayedDiscovered = IsFinalizable(object);
        });
        // Record the product discovery result before a severed producer lets
        // normal follow/reclamation obscure these already reached assertions.
        // The healthy arm continues through the unchanged enqueue assertions.
        if (failures.load() != 0) {
            std::printf("P2_REGISTRATION_RESULT failures=%u target_stage=next_old_mark\n", failures.load());
            std::fflush(stdout);
            std::_Exit(failures.load());
        }
    };
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    ZGeneration::testYoungMarkCompleted = nullptr;
    ZGeneration::testOldMarkStarted = nullptr;
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
    auto& references = heap.GetFinalizerProcessor().GetReferenceProcessor();
    BaseObject* holder = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* child = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* sentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* upgraded = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* control = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(holder, Slot(holder), child);
    ZBarrier::WriteReference(child, Slot(child), sentinel);
    ZBarrier::WriteReference(upgraded, Slot(upgraded), control);
    NativeSlot strongRoot(zpointer::null);
    ZBarrier::WriteStaticRef(strongRoot, upgraded);
    NativeSlot* roots[] = { &strongRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    NativeSlot setupRoot(zpointer::null);
    ZBarrier::WriteStaticRef(setupRoot, holder);
    NativeSlot* setupRoots[] = { &setupRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    heap.RequestGC(GC_REASON_USER, false);
    holder = ZBarrier::ReadStaticRef(setupRoot);
    child = ZBarrier::ReadReference(holder, Slot(holder));
    sentinel = ZBarrier::ReadReference(child, Slot(child));
    upgraded = ZBarrier::ReadStaticRef(strongRoot);
    control = ZBarrier::ReadReference(upgraded, Slot(upgraded));
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    holder->OnFinalizerCreated();
    upgraded->OnFinalizerCreated();

    const size_t discovered = references.Discovered(ReferenceType::FINAL);
    const size_t enqueued = references.Enqueued(ReferenceType::FINAL);
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "finalizable_product_mark_end_breakpoint");
    auto isFinal = [](BaseObject* object) {
        return IsFinalizable(object);
    };
    auto isStrong = [](BaseObject* object) {
        auto* page = Heap::page(reinterpret_cast<MAddress>(object));
        return page->is_strong_bit_set(from_object(object));
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
    auto& collector = heap;
    const bool finalizable = std::getenv("P2_ARRAY_FINALIZABLE") != nullptr;
    BaseObject* first = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* last = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* control = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* holder = MObject::NewObject(holderType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(holder, Slot(holder), control);
    const size_t length = 2 * MarkPartialArray::MIN_LENGTH + 17;
    const size_t fieldCount = length * (structArray ? 2 : 1);
    MArray* array = structArray ? MCC_NewArray(arrayType, length) : MCC_NewObjArray(arrayType, length);
    auto* elements = reinterpret_cast<RefField<>*>(reinterpret_cast<uint8_t*>(array) + sizeof(MArray));
    for (size_t index = 0; index < fieldCount; ++index) {
        ZBarrier::WriteReference(array, elements[index], index % 2 == 0 ? first : last);
    }
    ZBarrier::WriteReference(holder, Slot(holder), array);
    NativeSlot arrayRoot(zpointer::null), controlRoot(zpointer::null);
    ZBarrier::WriteStaticRef(arrayRoot, array);
    ZBarrier::WriteStaticRef(controlRoot, control);
    NativeSlot* roots[] = { &controlRoot, &arrayRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    NativeSlot holderRoot(zpointer::null);
    ZBarrier::WriteStaticRef(holderRoot, holder);
    NativeSlot* holderRoots[] = { &holderRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(holderRoots), 1);
    heap.RequestGC(GC_REASON_USER, false);
    holder = ZBarrier::ReadStaticRef(holderRoot);
    array = static_cast<MArray*>(ZBarrier::ReadStaticRef(arrayRoot));
    elements = reinterpret_cast<RefField<>*>(reinterpret_cast<uint8_t*>(array) + sizeof(MArray));
    first = ZBarrier::ReadReference(array, elements[0]);
    last = ZBarrier::ReadReference(array, elements[1]);
    control = ZBarrier::ReadStaticRef(controlRoot);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(holderRoots), 1);
    if (finalizable) {
        heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
        heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
        holder->OnFinalizerCreated();
    }
    std::atomic<size_t> fields{0};
    std::atomic<bool> rangeTarget{false};
    auto arrayAddress = [&] {
        return raw(finalizable ? Slot(holder).GetTargetObject() : arrayRoot.GetTargetObject());
    };
    ZBarrier::testFieldMarkResult = [&](ZBarrier::FieldMarkKind kind, RefField<>& field, zpointer, zaddress) {
        const auto expected = finalizable ? ZBarrier::FieldMarkKind::Finalizable : ZBarrier::FieldMarkKind::Old;
        if (kind != expected) return;
        auto* current = reinterpret_cast<MArray*>(arrayAddress());
        const MAddress begin = reinterpret_cast<MAddress>(reinterpret_cast<uint8_t*>(current) + sizeof(MArray));
        const MAddress address = reinterpret_cast<MAddress>(&field);
        if (address >= begin && address < begin + fieldCount * sizeof(uintptr_t)) {
            ++fields;
            if (address == begin + (fieldCount - 1) * sizeof(uintptr_t)) rangeTarget = true;
        }
    };
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"), "array_product_mark_end_breakpoint");
    auto* current = reinterpret_cast<MArray*>(arrayAddress());
    auto* firstPage = Heap::page(reinterpret_cast<MAddress>(first));
    auto* lastPage = Heap::page(reinterpret_cast<MAddress>(last));
    auto* controlPage = Heap::page(reinterpret_cast<MAddress>(control));
    Expect(!Heap::page(reinterpret_cast<MAddress>(current))->IsYoungRegion(), "array_real_old_owner");
    Expect(fields.load() == fieldCount, structArray ? "struct_array_visits_all_members" : "array_full_and_range_visit_all_fields");
    Expect(rangeTarget.load(), structArray ? "struct_array_last_member_reached" : "array_range_target_reached");
    Expect(finalizable ? IsFinalizable(first) :
        firstPage->is_strong_bit_set(from_object(first)), "array_first_child_retained_in_domain");
    Expect(finalizable ? IsFinalizable(last) :
        lastPage->is_strong_bit_set(from_object(last)), "array_last_child_retained_in_domain");
    Expect(controlPage->is_strong_bit_set(from_object(control)), "array_independent_strong_control");
    ZBarrier::testFieldMarkResult = nullptr;
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
class P2FieldInputTask final : public ZTask {
public:
    P2FieldInputTask(Heap& collector, std::function<void()> exercise)
        : ZTask("P2FieldInputTask"), collector(collector), exercise(std::move(exercise)) {}
    void work() override
    {
        if (!claimed.exchange(true)) exercise();
        // Same GC-worker tail protocol as MarkOldRootsTask. The field entry
        // produced these entries; the test never supplies mark/current output.
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *Heap::GetHeap().old().MarkPtr());
        Expect(Heap::GetHeap().old().MarkPtr()->Stacks().IsEmpty(), "slow_input_worker_tls_drained");
    }
private:
    Heap& collector;
    std::function<void()> exercise;
    std::atomic<bool> claimed{false};
};
}

// Field API input layer. Pause at old mark-start Complete: the combined
// pause starts young FIRST, old SECOND (zGeneration.cpp VM_ZMarkStartYoungAndOld).
// This point observes both real colour flips before remset scanning.
// Every word is from a real store; no masks, phases or mark results are seeded.
extern "C" int p2SlowFieldInputExercise()
{
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)]{};
    auto* holderType = Type(types[0], true, 2);
    holderType->SetSourceGeneric(reinterpret_cast<TypeTemplate*>(&P2Finalize));
    auto* edgeType = Type(types[1], true, 1);
    auto* leafType = Type(types[2], false, 1);
    auto& heap = Heap::GetHeap();
    auto& collector = static_cast<Heap&>(heap);
    BaseObject* strongHolder = MObject::NewObject(holderType, 24, AllocType::MOVEABLE_OBJECT);
    BaseObject* finalHolder = MObject::NewObject(holderType, 24, AllocType::MOVEABLE_OBJECT);
    BaseObject* oldChild = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* oldSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* finalChild = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* finalSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(oldChild, Slot(oldChild), oldSentinel);
    ZBarrier::WriteReference(finalChild, Slot(finalChild), finalSentinel);
    ZBarrier::WriteReference(strongHolder, Slot(strongHolder), oldChild);
    ZBarrier::WriteReference(strongHolder, Slot(strongHolder, 1), oldChild);
    ZBarrier::WriteReference(finalHolder, Slot(finalHolder), finalChild);
    ZBarrier::WriteReference(finalHolder, Slot(finalHolder, 1), finalChild);
    NativeSlot strongRoot(zpointer::null);
    ZBarrier::WriteStaticRef(strongRoot, strongHolder);
    NativeSlot* roots[] = { &strongRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    NativeSlot finalSetupRoot(zpointer::null);
    ZBarrier::WriteStaticRef(finalSetupRoot, finalHolder);
    NativeSlot* setupRoots[] = { &finalSetupRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    heap.RequestGC(GC_REASON_USER, false);
    strongHolder = ZBarrier::ReadStaticRef(strongRoot);
    finalHolder = ZBarrier::ReadStaticRef(finalSetupRoot);
    oldChild = ZBarrier::ReadReference(strongHolder, Slot(strongHolder, 1));
    oldSentinel = ZBarrier::ReadReference(oldChild, Slot(oldChild));
    finalChild = ZBarrier::ReadReference(finalHolder, Slot(finalHolder, 1));
    finalSentinel = ZBarrier::ReadReference(finalChild, Slot(finalChild));

    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    finalHolder->OnFinalizerCreated();
    Expect(!Heap::page(reinterpret_cast<MAddress>(strongHolder))->IsYoungRegion(), "slow_real_strong_holder_promoted");
    Expect(!Heap::page(reinterpret_cast<MAddress>(finalHolder))->IsYoungRegion(), "slow_real_final_holder_promoted");
    auto* young = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    auto* youngSentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(young, Slot(young), youngSentinel);
    RefField<>& strongYoungSlot = Slot(strongHolder);
    RefField<>& finalYoungSlot = Slot(finalHolder);
    RefField<>& strongOldSlot = Slot(strongHolder, 1);
    RefField<>& finalOldSlot = Slot(finalHolder, 1);
    ZBarrier::WriteReference(strongHolder, strongYoungSlot, young);
    ZBarrier::WriteReference(finalHolder, finalYoungSlot, young);
    const zpointer stored = strongYoungSlot.GetFieldValue();
    std::atomic<unsigned> strongSlow{0}, finalSlow{0}, strongFast{0}, finalFast{0};
    std::atomic<unsigned> strongFollow{0}, finalFollow{0};
    std::atomic<bool> inputTask{false};
    ZBarrier::testFieldMarkResult = [&](ZBarrier::FieldMarkKind kind, RefField<>& field, zpointer observed, zaddress result) {
        if (inputTask.load()) {
            if (&field == &strongYoungSlot) {
                ++strongSlow;
                Expect(!ZPointer::is_mark_good(observed), "strong_young_slow_input_selected");
                Expect(is_null(result), "strong_young_slow_no_object_result");
                Expect(field.GetFieldValue() == observed, "strong_young_slow_does_not_heal");
            } else if (&field == &finalYoungSlot) {
                const bool finalFastPath = ZPointer::is_load_good(observed) && ZPointer::is_marked_any_old(observed);
                std::printf("P2_FINAL_SLOW_OBS load_good=%d marked_any_old=%d mark_good=%d result_null=%d kind=%d\n",
                            ZPointer::is_load_good(observed), ZPointer::is_marked_any_old(observed),
                            ZPointer::is_mark_good(observed), is_null(result), static_cast<int>(kind));
                Expect(!finalFastPath, "final_young_slow_input_selected");
                ++finalSlow;
                Expect(is_null(result), "final_young_slow_no_object_result");
                Expect(field.GetFieldValue() == observed, "final_young_slow_does_not_heal");
            } else if (&field == &strongOldSlot) {
                Expect(to_object(result) == oldChild, "strong_old_slow_current_control");
                if (ZPointer::is_mark_good(observed)) {
                    ++strongFast;
                    Expect(field.GetFieldValue() == observed, "strong_old_fast_unchanged");
                }
            } else if (&field == &finalOldSlot) {
                Expect(to_object(result) == finalChild, "final_old_slow_current_control");
                if (ZPointer::is_load_good(observed) && ZPointer::is_marked_any_old(observed)) {
                    ++finalFast;
                    Expect(field.GetFieldValue() == observed, "final_old_fast_unchanged");
                }
            }
        }
        if (&field == &Slot(oldChild) && kind == ZBarrier::FieldMarkKind::Old) {
            ++strongFollow;
            Expect(to_object(result) == oldSentinel &&
                   Heap::page(raw(result))->is_strong_bit_set(result),
                   "slow_old_strong_control_followed_by_product");
        }
        if (&field == &Slot(finalChild) && kind == ZBarrier::FieldMarkKind::Finalizable) {
            ++finalFollow;
            // Inspect the mark result here, before non-strong processing and
            // relocation can reset the page's livemap (ZGC zPage.cpp:115-117).
            std::printf("P2_FINAL_CONTROL same=%d live=%d strong=%d observed=%#zx result=%#zx\n",
                        to_object(result) == finalSentinel,
                        Heap::page(raw(result))->is_live_bit_set(result),
                        Heap::page(raw(result))->is_strong_bit_set(result), raw(observed), raw(result));
            Expect(to_object(result) == finalSentinel && IsFinalizable(finalSentinel),
                   "slow_old_final_control_followed_by_product");
        }
    };
    unsigned started = 0;
    ZGeneration::testMarkStartState = [&](ZGenerationId generation, MarkStartPoint point, const ZMark*) {
        if (generation != ZGenerationId::old || point != MarkStartPoint::Complete) return;
        if (!heap.young().IsMajorRoots()) return;
        ++started;
        Expect(Slot(strongHolder).GetFieldValue() == stored, "slow_input_original_store_word_preserved");
        auto* page = Heap::page(reinterpret_cast<MAddress>(young));
        auto bit = [&] {
            return page->livemap().get(page->generation_id(), page->bit_index(from_object(young)) + 1);
        };
        const bool before = bit();
        P2FieldInputTask task(collector, [&] {
            auto& youngStacks = Heap::GetHeap().young().MarkPtr()->Stacks();
            const size_t youngBefore = youngStacks.Population();
            inputTask = true;
            ZBarrier::MarkBarrierOnOldOopField(strongHolder, strongYoungSlot, false);
            ZBarrier::MarkBarrierOnOldOopField(finalHolder, finalYoungSlot, true);
            ZBarrier::MarkBarrierOnOldOopField(strongHolder, strongOldSlot, false);
            ZBarrier::MarkBarrierOnOldOopField(finalHolder, finalOldSlot, true);
            ZBarrier::MarkBarrierOnOldOopField(strongHolder, strongOldSlot, false);
            ZBarrier::MarkBarrierOnOldOopField(finalHolder, finalOldSlot, true);
            inputTask = false;
            Expect(youngStacks.Population() == youngBefore, "slow_old_fields_do_not_publish_young_entries");
        });
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).Workers()->run(&task);
        Expect(bit() == before, "slow_old_fields_do_not_write_young_bitmap");
        // zMark.inline.hpp:16-47 (ZGC zMark.inline.hpp:48-87): mark_object
        // early-returns when the target is already marked, so a mark-stack
        // population assertion is not a valid invariant here. Real slow-path
        // processing is proven by the slow counters and the follow checks below.
        Expect(strongSlow == 1, "slow_input_strong_field_entry_reached");
        Expect(finalSlow == 1, "slow_input_final_field_entry_reached");
        Expect(strongFast == 1 && finalFast == 1, "slow_input_legal_fast_controls_reached");
        if (failures.load() != 0) {
            std::printf("P2_SLOW_RESULT failures=%u target_stage=field_result\n", failures.load());
            std::fflush(stdout);
            std::_Exit(failures.load());
        }
    };
    Heap::GetHeap().RequestGC(GC_REASON_HEU_SYNC, false);
    ZGeneration::testMarkStartState = nullptr;
    ZBarrier::testFieldMarkResult = nullptr;
    Expect(started == 1, "slow_input_real_major_roots_phase_reached");
    Expect(strongFollow != 0, "slow_strong_control_result_observed");
    Expect(finalFollow != 0, "slow_final_control_result_observed");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_SLOW_RESULT failures=%u strong=%u final=%u strong_follow=%u final_follow=%u\n",
                failures.load(), strongSlow.load(), finalSlow.load(), strongFollow.load(), finalFollow.load());
    return failures.load();
}

// A real minor while the major driver is paused gives the nonmajor field an
// unmarked old target. Comparing an already-marked target alone would miss a
// spurious old mark (ZGC zBarrier.cpp:158-183). Hooks schedule; they seed no state.
extern "C" int p2MinorDuringOldMarkExercise()
{
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* edgeType = Type(types[0], true, 2);
    auto* leafType = Type(types[1], false, 1);
    auto& heap = Heap::GetHeap();
    BaseObject* holder = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
    BaseObject* target = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    BaseObject* control = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(holder, Slot(holder), target);
    ZBarrier::WriteReference(holder, Slot(holder, 1), control);
    NativeSlot setup(zpointer::null);
    ZBarrier::WriteStaticRef(setup, holder);
    NativeSlot* setupRoots[] = { &setup };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    heap.RequestGC(GC_REASON_USER, false);
    holder = ZBarrier::ReadStaticRef(setup);
    target = ZBarrier::ReadReference(holder, Slot(holder));
    control = ZBarrier::ReadReference(holder, Slot(holder, 1));

    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(setupRoots), 1);
    ConcurrentGCBreakpoints::AcquireControl();
    Expect(ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED"), "minor_during_old_product_breakpoint");
    auto* child = MObject::NewObject(edgeType, 24, AllocType::MOVEABLE_OBJECT);
    ZBarrier::WriteReference(child, Slot(child), target);
    NativeSlot childRoot(zpointer::null);
    ZBarrier::WriteStaticRef(childRoot, child);
    NativeSlot* roots[] = { &childRoot };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    unsigned selected = 0;
    auto marked = [&](BaseObject* object) {
        return Heap::page(reinterpret_cast<MAddress>(object))->is_strong_bit_set(from_object(object));
    };
    ZGeneration::testYoungMarkStarted = [&] {
        Expect(!heap.young().IsMajorRoots() &&
               heap.GetCycleSnapshot(ZGenerationId::old).phase == ZGenerationPhase::Mark, "minor_during_old_real_phase_axis");
        Expect(!Heap::page(reinterpret_cast<MAddress>(target))->IsYoungRegion(), "minor_during_old_real_old_target");
        Expect(!marked(target) && !marked(control), "minor_during_old_unmarked_inputs");
        bool observing = true;
        ZBarrier::testFieldMarkResult = [&](ZBarrier::FieldMarkKind kind, RefField<>& field,
                                          zpointer observed, zaddress result) {
            if (observing && kind == ZBarrier::FieldMarkKind::Young && &field == &Slot(child)) {
                ++selected;
                Expect(!ZPointer::is_store_good(observed), "minor_during_old_slow_selected");
                Expect(to_object(result) == target, "minor_during_old_returns_current");
                Expect(!marked(target), "minor_during_old_does_not_mark_old");
                Expect(ZPointer::is_store_good(field.GetFieldValue()), "minor_during_old_heals_store_good");
            }
        };
        P2FieldInputTask task(heap, [&] {
            ZBarrier::MarkBarrierOnYoungOopField(Slot(child));
            ZBarrier::MarkBarrierOnOldOopField(holder, Slot(holder, 1), false);
            Expect(marked(control), "minor_during_old_strong_positive_control");
            // Retain the negative target before resuming the major, through the
            // normal old field entry, after the nonmajor assertion has read it.
            ZBarrier::MarkBarrierOnOldOopField(holder, Slot(holder), false);
        });
        heap.old().Workers()->run(&task);
        observing = false;
        ZBarrier::testFieldMarkResult = nullptr;
    };
    heap.RequestGC(GC_REASON_YOUNG, false);
    ZGeneration::testYoungMarkStarted = nullptr;
    Expect(selected == 1, "minor_during_old_field_result_observed");
    std::printf("P2_MINOR_RESULT failures=%u selected=%u\n", failures.load(), selected);
    std::fflush(stdout);
    if (failures.load() != 0) std::_Exit(failures.load());
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    return failures.load();
}

// Advisor 105847Z: binding qualification is separate from the remset liveness
// test. The negative child calls the same product registration with no binding.
extern "C" int p2RemsetBindingExercise()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = Type(storage, false, 1);
    auto& heap = Heap::GetHeap();
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, MObject::NewObject(type, 16, AllocType::MOVEABLE_OBJECT));
    NativeSlot* roots[] = { &root };
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    heap.RequestGC(GC_REASON_USER, false);
    auto* page = Heap::page(reinterpret_cast<MAddress>(ZBarrier::ReadStaticRef(root)));
    Expect(!page->IsYoungRegion(), "binding_real_old_page");
    if (failures.load() != 0) return failures.load();
    std::fflush(nullptr);
    const pid_t child = fork();
    Expect(child >= 0, "binding_negative_child_started");
    if (child == 0) {
        ZRemembered unbound(nullptr, &heap.old().forwarding_table(), &heap.page_allocator());
        unbound.register_found_old(page);
        std::_Exit(0);
    }
    if (child > 0) {
        int status = 0;
        Expect(waitpid(child, &status, 0) == child, "binding_negative_child_waited");
        Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
               "binding_unbound_registration_rejected");
    }
    ZRemembered bound(&Heap::page_table(), &generation_forwarding_table(Generation::Old), &heap.page_allocator());
    bound.register_found_old(page);
    ZRemsetTableIterator iter(&bound, false);
    ZRemsetTableEntry entry{};
    Expect(iter.next(&entry) && entry._page == page, "binding_registered_page_reaches_iterator");
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::printf("P2_BINDING_RESULT failures=%u\n", failures.load());
    return failures.load();
}
