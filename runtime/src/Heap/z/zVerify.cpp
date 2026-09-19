// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zVerify.hpp"
#include <cstdlib>
#include <cstring>
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "TypeInfoManager.h"
#include <unordered_set>
#include <vector>
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
// Cangjie's rendezvous protocol uses the mutator safe-region state. GC and
// worker threads carry GC_THREAD; mutators block rendezvous outside saferegions.
// ZGC zVerify.cpp:63-110.
void z_verify_safepoints_are_blocked()
{
    if (ThreadLocal::GetThreadType() == ThreadType::GC_THREAD) { return; }
    Mutator* const mutator = ThreadLocal::GetMutator();
    DCHECK(MutatorManager::Instance().WorldStopped() ||
           (mutator != nullptr && !mutator->InSaferegion()));
}
#endif

namespace {
// VM adapter: the first WeakRef payload slot is outside Cangjie's ordinary
// strong-field bitmap. HotSpot's reference Klass dispatch owns that layout.
// This is raw iteration, as at zVerify.cpp:632 and 737, not a safe split.
template <typename Function>
void IterateVerifyFields(BaseObject* object, Function function)
{
    const MAddress referent = reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE;
    if (object->IsWeakRef()) { function(HeapSlotAt<>(referent)); }
    object->ForEachRefField([&](RefField<>& field) {
        if (!object->IsWeakRef() || reinterpret_cast<MAddress>(&field) != referent) {
            function(field);
        }
    });
}

#if defined(MRT_DEBUG) && MRT_DEBUG == 1
constexpr bool trueInDebug = true;
#else
constexpr bool trueInDebug = false;
#endif
bool Flag(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    return value == nullptr ? defaultValue : std::strcmp(value, "1") == 0;
}
}
const bool ZVerifyRoots = Flag("ZVerifyRoots", trueInDebug);
const bool ZVerifyObjects = Flag("ZVerifyObjects", false);
const bool ZVerifyMarking = Flag("ZVerifyMarking", trueInDebug);
const bool ZVerifyRemembered = Flag("ZVerifyRemembered", trueInDebug);
const bool ZVerifyForwarding = Flag("ZVerifyForwarding", false);
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
const bool ZVerifyOops = Flag("ZVerifyOops", false);
#else
// Upstream develop flags are unavailable in a product build.
const bool ZVerifyOops = false;
#endif

// zVerify.cpp:489-515: phase entrypoints, not a GC-wide diagnostic scene.
void ZVerify::BeforeZOperation()
{
    if (ZVerifyRoots) { RootsStrong(false); }
}
void ZVerify::AfterMark()
{
    if (ZVerifyRoots) { RootsStrong(true); }
    if (ZVerifyObjects) { Objects(false); }
}
void ZVerify::AfterWeakProcessing()
{
    if (ZVerifyRoots) {
        RootsStrong(true);
        RootsWeak();
    }
    if (ZVerifyObjects) { Objects(true); }
}
} // namespace MapleRuntime

namespace MapleRuntime {

namespace {
// zVerify.cpp:206-256. Do not normalize raw roots before verifying them.
void ColoredRoot(NativeSlot& root, bool afterOldMark)
{
    DCHECK(!Heap::IsHeapAddress(&root));
    const zpointer value = root.GetFieldValue(std::memory_order_acquire);
    if (!(!is_null_any(to_zpointer(raw(value))))) { return; }
    CHECK_DETAIL(ClassifySlotWord(raw(value)) != SlotWordVerdict::kIllegal, "Bad colored root at %p", &root);
    if (afterOldMark) {
        CHECK_DETAIL(ZPointer::is_marked_old(to_zpointer(raw(value))),
                     "Unmarked old root at %p", &root);
    }
    ZVerify::Object(ZBarrier::ReadStaticRef(root), &root);
}
void PlainRoot(ObjectRef& root)
{
    DCHECK(!Heap::IsHeapAddress(&root));
    const uintptr_t value = raw(root.LoadPlain(std::memory_order_acquire));
    if (value == 0) { return; }
    // Object checks the uncolored address before it is dereferenced.
    ZVerify::Object(reinterpret_cast<BaseObject*>(value), &root);
}
}
void ZVerify::RootsStrong(bool afterOldMark)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    RootsIteratorStrongColored().Apply([&](NativeSlot& root) { ColoredRoot(root, afterOldMark); });
    ZMark::VisitStrongPlainRoots(PlainRoot, [](Mutator& mutator) {
        mutator.VisitProcessedRoots([&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, PlainRoot);
        });
    });
}
void ZVerify::RootsWeak()
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!ZResurrection::is_blocked());
    RootsIteratorWeakColored().Apply([](NativeSlot& root) { ColoredRoot(root, true); });
}


namespace { BaseObject* brokenObject = nullptr; }
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
void VerifyAccessedOop(zaddress address)
{
    // Deliberately avoid to_object: this is that conversion's verification leaf.
    ZVerify::Object(reinterpret_cast<BaseObject*>(raw(address)), nullptr);
}
#endif

// zVerify.cpp:119-128. Check real addresses, before reading object metadata.
void ZVerify::Object(BaseObject* object, const void* slot)
{
    const uintptr_t addr = reinterpret_cast<uintptr_t>(object);
    CHECK_DETAIL(addr != 0 && is_valid(static_cast<zaddress>(addr)) &&
                 (addr & (alignof(void*) - 1)) == 0 && Heap::IsHeapAddress(addr),
                 "Bad object %p found at %p", object, slot);
    ZPage* region = Heap::page(addr);
    CHECK_DETAIL(region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion(),
                 "Bad object page %p found at %p", object, slot);
    TypeInfo* type = object->GetTypeInfo();
    CHECK_DETAIL(TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(
                     reinterpret_cast<uintptr_t>(type)) && object->IsValidObject() && type->IsVaildType(),
                 "Bad object type %p found at %p", object, slot);
}

// zVerify.cpp:131-203. Strong old edges and weak-inclusive edges differ in
// both markedness and remembered-set obligations. Always resolve a stale oop
// through the load barrier before checking its real object address.
void ZVerify::Oop(BaseObject* base, RefField<>& field, bool verifyWeaks)
{
    const zpointer value = field.GetFieldValue(std::memory_order_acquire);
    if (!verifyWeaks && value == zpointer::null) {
        // zVerify.cpp:133-136: raw null is only possible when flip promoting.
        CHECK_DETAIL(ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark_complete(),
                     "Raw null requires young mark complete at %p", &field);
        ZPage* holder = Heap::page(reinterpret_cast<MAddress>(base));
        // ZPage::is_allocating (zPage.inline.hpp:180-182).
        CHECK_DETAIL(holder->IsAllocating(),
                     "Raw null requires allocating holder at %p", &field);
    }
    if (!(!is_null_any(to_zpointer(raw(value))))) { return; }
    RefField<> preloaded(value);
    if (!verifyWeaks && ZPointer::is_mark_good(preloaded.GetFieldValue())) {
        Object(to_object(preloaded.GetTargetObject()), &field);
        return;
    }
    BaseObject* target = ZBarrier::ReadReference(base, field);
    Object(target, &field);
    const bool young = Heap::page(reinterpret_cast<MAddress>(target))->IsYoungRegion();
    if (verifyWeaks) {
        CHECK_DETAIL(ZPointer::is_marked_any_old(to_zpointer(raw(value))),
                     "Bad possibly weak oop at %p", &field);
        CHECK_DETAIL(!young || ZPointer::is_marked_young(to_zpointer(raw(value))),
                     "Unmarked young oop at %p", &field);
        CHECK_DETAIL(young || Heap::page(reinterpret_cast<MAddress>(target))->is_object_live(
                                  from_object(target)),
                     "Non-live old oop at %p", &field);
        const uintptr_t remset = raw(value) & ZPointerRememberedMask;
        const uintptr_t previous = (::g_cjStoreGoodMask & ZPointerRememberedMask) ^ ZPointerRememberedMask;
        CHECK_DETAIL(remset != previous, "Previous remembered color at %p", &field);
        CHECK_DETAIL(remset == ZPointerRememberedMask ||
                      Heap::page(reinterpret_cast<MAddress>(&field))->is_remembered(
                          reinterpret_cast<volatile zpointer*>(&field)) ||
                     MutatorManager::Instance().StoreBarrierBufferContains(reinterpret_cast<MAddress>(&field)),
                     "Missing remembered field at %p", &field);
    } else {
        const bool youngMarking = ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark();
        if (!young || !youngMarking) {
            CHECK_DETAIL(ZPointer::is_marked_old(to_zpointer(raw(value))),
                         "Unmarked old oop at %p", &field);
            CHECK_DETAIL(!Heap::page(reinterpret_cast<MAddress>(base))->IsYoungRegion(),
                         "Old oop holder must be old at %p", &field);
        }
    }
}

void ZVerify::threads_start_processing()
{
    JavaThreadsIterator threads;
    const uint64_t epoch = StackWatermark::epoch_id();
    if (epoch == 0) {
        return;
    }
    RootVisitor noop = [](ObjectRef&) {};
    threads.Apply([&](Mutator& mutator) {
        size_t frames = 0;
        (void)StackWatermarkSet::finish_processing(mutator, noop, noop, epoch, nullptr, frames);
    });
}

// zVerify.cpp:397-487. Visit the root-reachable graph, including young bridge
// objects, but check outgoing fields only on live old objects.
void ZVerify::Objects(bool verifyWeaks)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!ZResurrection::is_blocked());
    if (ZAbort::should_abort()) { return; }
    DCHECK((ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark_complete()) ||
           (ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_mark_complete()));
    threads_start_processing();
    BaseObject* visitedBase = nullptr;
    const void* visitedSlot = nullptr;
    uintptr_t visitedValue = 0;
    HeapIterator iterator(verifyWeaks, true);
    iterator.Iterate([&](BaseObject* object) {
        Object(object, nullptr);
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) { return; }
        if (!region->is_object_live(from_object(object))) {
            LOG(RTLOG_ERROR, "ZVerify found non-live object: %p at %p value=%#zx from=%p",
                object, visitedSlot, visitedValue, visitedBase);
            if (brokenObject == nullptr) { brokenObject = object; }
            return;
        }
        const MAddress referent = reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE;
        if (object->IsWeakRef() && verifyWeaks) { Oop(object, HeapSlotAt<>(referent), verifyWeaks); }
        RefFieldVisitor fields = [&](RefField<>& field) {
            if (!object->IsWeakRef() || reinterpret_cast<MAddress>(&field) != referent) {
                Oop(object, field, verifyWeaks);
            }
        };
        ZBasicOopIterateClosure<RefFieldVisitor> closure(fields);
        // zVerify.cpp:426-429: live-object verification uses the safe entry.
        ZIterator::oop_iterate_safe(object, &closure);
    }, [&](BaseObject* base, const void* slot, uintptr_t value) {
        visitedBase = base;
        visitedSlot = slot;
        visitedValue = value;
    });
    // zVerify.cpp:504 asserts the accumulated old-mark result; the later weak
    // verification logs any non-live reachable objects without this assertion.
    if (!verifyWeaks) { CHECK_DETAIL(brokenObject == nullptr, "Object verification failed"); }
}


namespace {
// zVerify.cpp:521-523. Replaced only at a color-flip safepoint.
std::unordered_set<MAddress> bufferedStores;
bool IntentionallyUnremembered(zpointer value)
{
    // The upstream exemption is both remembered bits, not just the current bit.
    return (raw(value) & ZPointerRememberedMask) == ZPointerRememberedMask;
}
}
void ZVerify::OnColorFlip()
{
    if (!ZVerifyRemembered || !kBufferStoreBarriers) { return; }
    bufferedStores.clear();
    MutatorManager::Instance().VisitStoreBarrierBuffers([](MAddress slot) { bufferedStores.insert(slot); });
}

// zVerify.cpp:531-609. Source-page verification is old-to-old only.
void ZVerify::BeforeRelocation(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr ||
        forwarding->from_age() != PageAge::old) { return; }
    ZPage* page = forwarding->page();
    if (page == nullptr) { return; }
    const bool activeCurrent = Heap::GetHeap().OldActiveRemsetIsCurrent();
    if (activeCurrent) {
        page->verify_remset_cleared_previous();
    } else {
        page->verify_remset_cleared_current();
    }
    // zVerify.cpp:601 forwarding->object_iterate: the source page livemap.
    page->object_iterate([&](BaseObject* object) {
        const MAddress from = reinterpret_cast<MAddress>(object);
        IterateVerifyFields(object, [&](RefField<>& field) {
            const MAddress slot = reinterpret_cast<MAddress>(&field);
            if (IntentionallyUnremembered(field.GetFieldValue()) || bufferedStores.count(slot) != 0 ||
                forwarding->find(from) != 0) { return; }
            CHECK_DETAIL(activeCurrent
                             ? page->is_remembered(reinterpret_cast<volatile zpointer*>(slot))
                             : page->was_remembered(reinterpret_cast<volatile zpointer*>(slot)),
                         "Missing remembered field %p in source %p", &field, object);
        });
    });
}

// zVerify.cpp:610-738. Recheck the pointer after reading both bitmap faces;
// a concurrent scanner may have self-healed the pointer and cleared the bit.
void ZVerify::AfterRelocationInternal(ZForwarding* forwarding)
{
    std::vector<MAddress> fromAddresses;
    forwarding->for_each_from([&](MAddress from) { fromAddresses.push_back(from); });
    for (MAddress from : fromAddresses) {
        BaseObject* object = reinterpret_cast<BaseObject*>(forwarding->find(from));
        Object(object, nullptr);
        // Destination age is represented by the destination page in this port.
        if (Heap::page(reinterpret_cast<MAddress>(object))->IsYoungRegion()) { continue; }
        IterateVerifyFields(object, [&](RefField<>& field) {
            const MAddress slot = reinterpret_cast<MAddress>(&field);
            const zpointer value = field.GetFieldValue(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);
            RefField<> preloaded(value);
            if (IntentionallyUnremembered(value) || ZPointer::is_store_good(preloaded.GetFieldValue()) ||
                bufferedStores.count(slot) != 0 ||
                bufferedStores.count(from + slot - reinterpret_cast<MAddress>(object)) != 0) { return; }
            ZPage* toPage = Heap::page(slot);
            if (toPage != nullptr &&
                (toPage->is_remembered(reinterpret_cast<volatile zpointer*>(slot)) ||
                 toPage->was_remembered(reinterpret_cast<volatile zpointer*>(slot)))) {
                return;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (field.GetFieldValue(std::memory_order_acquire) != value) { return; }
            CHECK_DETAIL(ZForwarding::young_marking(), "Missing remembered field outside young marking: %p", &field);
            CHECK_DETAIL(forwarding->relocated_remembered_fields_published_contains(slot),
                         "Missing published remembered field %p in destination %p", &field, object);
        });
    }
}
void ZVerify::AfterRelocation(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr) { return; }
    if (forwarding->to_age() != PageAge::old) { return; }
    if (ZForwarding::young_marking() && forwarding->relocated_remembered_fields_is_concurrently_scanned()) { return; }
    AfterRelocationInternal(forwarding);
}
void ZVerify::AfterScan(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr ||
        ZAbort::should_abort()) { return; }
    if ((ZGeneration::old() == nullptr || !ZGeneration::old()->is_phase_relocate()) ||
        !forwarding->relocated_remembered_fields_is_concurrently_scanned()) { return; }
    AfterRelocationInternal(forwarding);
}

} // namespace MapleRuntime
