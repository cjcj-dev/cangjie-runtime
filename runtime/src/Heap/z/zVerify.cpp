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
#include <unordered_set>
#include <vector>
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {
namespace { BaseObject* brokenObject = nullptr; }
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
    if (ZVerifyObjects) {
        Objects(false);
        CHECK_DETAIL(brokenObject == nullptr, "Object verification failed");
    }
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
void z_verify_oop_object(zaddress address, zpointer value, const void* slot);
void z_verify_root_oop_object(zaddress address, const void* slot);
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
    z_verify_root_oop_object(from_object(ZBarrier::ReadStaticRef(root)), &root);
}
void PlainRoot(ObjectRef& root)
{
    DCHECK(!Heap::IsHeapAddress(&root));
    const uintptr_t value = raw(root.LoadPlain(std::memory_order_acquire));
    if (value == 0) { return; }
    // Object checks the uncolored address before it is dereferenced.
    z_verify_root_oop_object(static_cast<zaddress>(value), &root);
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


namespace {
// ZGC zVerify.cpp:121-129, zHeap.cpp:167-191: verify the allocated page
// interval before any metadata access. Object metadata is not part of is_oop.
bool IsOop(zaddress address)
{
    const uintptr_t addr = raw(address);
    return addr != 0 && is_valid(address) && Heap::is_in(addr);
}
void z_verify_oop_object(zaddress address, zpointer value, const void* slot)
{
    CHECK_DETAIL(IsOop(address), "Bad object %#zx found at %p", raw(value), slot);
}
void z_verify_root_oop_object(zaddress address, const void* slot)
{
    CHECK_DETAIL(IsOop(address), "Bad object %#zx found at %p", raw(address), slot);
}

void z_verify_old_oop(RefField<>* field)
{
    const zpointer value = field->GetFieldValue(std::memory_order_acquire);
    if (value == zpointer::null) {
        CHECK_DETAIL(ZGeneration::young()->is_phase_mark_complete(),
                     "Raw null requires young mark complete at %p", field);
        CHECK_DETAIL(Heap::page(reinterpret_cast<MAddress>(field))->IsAllocating(),
                     "Raw null requires allocating holder at %p", field);
    }
    if (!is_null_any(value)) {
        if (ZPointer::is_mark_good(value)) {
            z_verify_oop_object(ZPointer::uncolor(value), value, field);
        } else {
            const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, value);
            if (Heap::is_old(raw(address)) || !ZGeneration::young()->is_phase_mark()) {
                CHECK_DETAIL(ZPointer::is_marked_old(value), "Unmarked old oop at %p", field);
                CHECK_DETAIL(Heap::is_old(reinterpret_cast<MAddress>(field)),
                             "Old oop holder must be old at %p", field);
            }
        }
    }
}

void z_verify_possibly_weak_oop(RefField<>* field)
{
    const zpointer value = field->GetFieldValue(std::memory_order_acquire);
    if (is_null_any(value)) { return; }
    CHECK_DETAIL(ZPointer::is_marked_any_old(value), "Bad possibly weak oop at %p", field);
    const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, value);
    const bool young = Heap::is_young(raw(address));
    CHECK_DETAIL(!young || ZPointer::is_marked_young(value), "Unmarked young oop at %p", field);
    CHECK_DETAIL(young || Heap::page(raw(address))->is_object_live(address),
                 "Non-live old oop at %p", field);
    z_verify_oop_object(address, value, field);
    const uintptr_t remset = raw(value) & ZPointerRememberedMask;
    const uintptr_t previous = (::g_cjStoreGoodMask & ZPointerRememberedMask) ^ ZPointerRememberedMask;
    CHECK_DETAIL(remset != previous, "Previous remembered color at %p", field);
    CHECK_DETAIL(remset == ZPointerRememberedMask ||
                 Heap::page(reinterpret_cast<MAddress>(field))->is_remembered(
                     reinterpret_cast<volatile zpointer*>(field)) ||
                 MutatorManager::Instance().StoreBarrierBufferContains(reinterpret_cast<MAddress>(field)),
                 "Missing remembered field at %p", field);
}
}
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
void VerifyAccessedOop(zaddress address)
{
    z_verify_root_oop_object(address, nullptr);
}
#endif

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

namespace {
class ZVerifyObjectClosure : public ObjectClosure, public OopFieldClosure {
    const bool verifyWeaks;
    BaseObject* visitedBase = nullptr;
    const void* visitedSlot = nullptr;
    uintptr_t visitedValue = 0;
public:
    explicit ZVerifyObjectClosure(bool verifyWeaks) : verifyWeaks(verifyWeaks) {}

    void log_dead_object(BaseObject* object)
    {
        LOG(RTLOG_ERROR, "ZVerify found non-live object: %p at %p value=%#zx from=%p",
            object, visitedSlot, visitedValue, visitedBase);
        LOG(RTLOG_ERROR, "object=%p type=%p size=%zu", object, object->GetTypeInfo(), object->GetSize());
        if (visitedBase != nullptr) {
            LOG(RTLOG_ERROR, "from=%p type=%p size=%zu", visitedBase,
                visitedBase->GetTypeInfo(), visitedBase->GetSize());
        }
        if (brokenObject == nullptr) { brokenObject = object; }
    }

    void verify_live_object(BaseObject* object)
    {
        const MAddress referent = reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE;
        if (object->IsWeakRef() && verifyWeaks) {
            z_verify_possibly_weak_oop(&HeapSlotAt<>(referent));
        }
        RefFieldVisitor fields = [&](RefField<>& field) {
            if (!object->IsWeakRef() || reinterpret_cast<MAddress>(&field) != referent) {
                if (verifyWeaks) { z_verify_possibly_weak_oop(&field); }
                else { z_verify_old_oop(&field); }
            }
        };
        ZBasicOopIterateClosure<RefFieldVisitor> closure(fields);
        ZIterator::oop_iterate_safe(object, &closure);
    }

    void do_object(BaseObject* object) override
    {
        z_verify_root_oop_object(from_object(object), nullptr);
        ZPage* page = Heap::page(reinterpret_cast<MAddress>(object));
        if (page->IsYoungRegion()) { return; }
        if (page->is_object_live(from_object(object))) { verify_live_object(object); }
        else { log_dead_object(object); }
    }

    void do_field(BaseObject* base, const void* slot, uintptr_t value) override
    {
        visitedBase = base;
        visitedSlot = slot;
        visitedValue = value;
    }
};
}

void ZVerify::Objects(bool verifyWeaks)
{
    if (ZAbort::should_abort()) { return; }
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK((ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark_complete()) ||
           (ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_mark_complete()));
    DCHECK(!ZResurrection::is_blocked());
    threads_start_processing();
    ZVerifyObjectClosure closure(verifyWeaks);
    Heap::GetHeap().object_and_field_iterate_for_verify(&closure, &closure, verifyWeaks);
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
    forwarding->for_each_from([&](MAddress from) {
        ZGeneration* generation = forwarding->from_age() == PageAge::old
            ? static_cast<ZGeneration*>(ZGeneration::old()) : static_cast<ZGeneration*>(ZGeneration::young());
        BaseObject* object = generation->remap_object(reinterpret_cast<BaseObject*>(from));
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
    });
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
