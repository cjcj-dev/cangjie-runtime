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
#include "Heap/z/zGeneration.inline.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/RegionSpace.h"
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
bool z_is_null_relaxed(zpointer value)
{
    return (raw(value) & ~(ZPointerAllMetadataMask | ZPointerReservedMask)) == 0;
}
void z_verify_oop_object(zaddress address, zpointer value, const void* slot);
void z_verify_root_oop_object(zaddress address, const void* slot);
// zVerify.cpp:206-256. Do not normalize raw roots before verifying them.
class ZVerifyColoredRootClosure {
    const bool afterOldMark;
public:
    explicit ZVerifyColoredRootClosure(bool afterOldMark) : afterOldMark(afterOldMark) {}
    void do_oop(NativeSlot& root)
    {
        DCHECK(!Heap::IsHeapAddress(&root));
        const zpointer value = root.GetFieldValue(std::memory_order_acquire);
        if (z_is_null_relaxed(value)) { return; }
        DCHECK(is_valid(value));
        if (afterOldMark) {
            CHECK_DETAIL(ZPointer::is_marked_old(value), "Unmarked old root at %p", &root);
            const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, value);
            z_verify_oop_object(address, value, &root);
        } else if (is_valid(value)) {
            const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, value);
            z_verify_oop_object(address, value, &root);
        }
    }
};
class ZVerifyUncoloredRootClosure {
public:
    void do_oop(ObjectRef& root)
    {
        DCHECK(!Heap::IsHeapAddress(&root));
        const uintptr_t value = raw(root.LoadPlain(std::memory_order_acquire));
        if (value == 0) { return; }
        z_verify_root_oop_object(static_cast<zaddress>(value), &root);
    }
};
}

void ZVerify::RootsStrong(bool afterOldMark)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    ZVerifyColoredRootClosure colored(afterOldMark);
    RootsIteratorStrongColored().Apply([&](NativeSlot& root) { colored.do_oop(root); });
    ZVerifyUncoloredRootClosure uncolored;
    RootVisitor plain = [&](ObjectRef& root) { uncolored.do_oop(root); };
    ZMark::VisitStrongPlainRoots(plain, [&](Mutator& mutator) {
        mutator.VisitProcessedRoots([&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, plain);
        });
    });
}
void ZVerify::RootsWeak()
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!ZResurrection::is_blocked());
    ZVerifyColoredRootClosure colored(true);
    RootsIteratorWeakColored().Apply([&](NativeSlot& root) { colored.do_oop(root); });
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
    if (!z_is_null_relaxed(value)) {
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
    if (z_is_null_relaxed(value)) { return; }
    CHECK_DETAIL(ZPointer::is_marked_old(value) || ZPointer::is_marked_finalizable(value),
                 "Bad possibly weak oop at %p", field);
    const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(nullptr, value);
    CHECK_DETAIL(Heap::is_old(raw(address)) || ZPointer::is_marked_young(value),
                 "Unmarked young oop at %p", field);
    CHECK_DETAIL(Heap::is_young(raw(address)) || Heap::GetHeap().IsSurvivedObject(to_object(address)),
                 "Non-live old oop at %p", field);
    z_verify_oop_object(address, value, field);
    const uintptr_t remset = raw(value) & ZPointerRememberedMask;
    const uintptr_t previous = ZPointerRemembered ^ ZPointerRememberedMask;
    CHECK_DETAIL(remset != previous, "Previous remembered color at %p", field);
    CHECK_DETAIL(remset == ZPointerRememberedMask ||
                 ZGeneration::young()->is_remembered(reinterpret_cast<volatile zpointer*>(field)) ||
                 StoreBarrierBuffer::is_in(reinterpret_cast<MAddress>(field)),
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
class ZVerifyOldOopClosure : public BasicOopIterateClosure {
    const bool verifyWeaks;
public:
    explicit ZVerifyOldOopClosure(bool verifyWeaks) : verifyWeaks(verifyWeaks) {}
    void do_oop(RefField<>* field) override
    {
        if (verifyWeaks) { z_verify_possibly_weak_oop(field); }
        else { z_verify_old_oop(field); }
    }
};

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
        ZVerifyOldOopClosure oopClosure(verifyWeaks);
        const MAddress referent = reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE;
        if (object->IsWeakRef() && verifyWeaks) { oopClosure.do_oop(&HeapSlotAt<>(referent)); }
        RefFieldVisitor fields = [&](RefField<>& field) {
            if (!object->IsWeakRef() || reinterpret_cast<MAddress>(&field) != referent) {
                oopClosure.do_oop(&field);
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

namespace {
// ZGC zVerify.cpp:531-609: keep field routing in the oop closure.
class ZVerifyRemsetBeforeOopClosure : public BasicOopIterateClosure {
    ZForwarding* const forwarding;
    MAddress from = 0;
public:
    explicit ZVerifyRemsetBeforeOopClosure(ZForwarding* value) : forwarding(value) {}
    void set_from_addr(MAddress value) { from = value; }
    void do_oop(RefField<>* pointer) override
    {
        RefField<>& field = *pointer;
        const MAddress slot = reinterpret_cast<MAddress>(pointer);
        ZPage* page = forwarding->page();
        if (IntentionallyUnremembered(field.GetFieldValue())) { return; }
        if (kBufferStoreBarriers && bufferedStores.count(slot) != 0) { return; }
        if (forwarding->find(from) != 0) { return; }
        CHECK_DETAIL(Heap::GetHeap().OldActiveRemsetIsCurrent()
                         ? page->is_remembered(reinterpret_cast<volatile zpointer*>(slot))
                         : page->was_remembered(reinterpret_cast<volatile zpointer*>(slot)),
                     "Missing remembered field %p in source %p", &field, reinterpret_cast<BaseObject*>(from));

    }
};
// ZGC zVerify.cpp:636-722: from/to identity stays with the field closure.
class ZVerifyRemsetAfterOopClosure : public BasicOopIterateClosure {
    ZForwarding* const forwarding;
    MAddress from = 0;
    MAddress to = 0;
public:
    explicit ZVerifyRemsetAfterOopClosure(ZForwarding* value) : forwarding(value) {}
    void set_from_addr(MAddress value) { from = value; }
    void set_to_addr(MAddress value) { to = value; }
    void do_oop(RefField<>* pointer) override
    {
        RefField<>& field = *pointer;
        const MAddress slot = reinterpret_cast<MAddress>(pointer);
        const zpointer value = field.GetFieldValue(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (IntentionallyUnremembered(value)) { return; }
        if (ZPointer::is_store_good(value)) { return; }
        if (kBufferStoreBarriers && bufferedStores.count(slot) != 0) { return; }
        if (kBufferStoreBarriers && bufferedStores.count(from + slot - to) != 0) { return; }
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
                     "Missing published remembered field %p in destination %p", &field, reinterpret_cast<BaseObject*>(to));

    }
};
}
void ZVerify::BeforeRelocation(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr || forwarding->from_age() != PageAge::old) { return; }
    ZPage* page = forwarding->page();
    if (Heap::GetHeap().OldActiveRemsetIsCurrent()) { page->verify_remset_cleared_previous(); }
    else { page->verify_remset_cleared_current(); }
    ZVerifyRemsetBeforeOopClosure closure(forwarding);
    page->object_iterate([&](BaseObject* object) {
        closure.set_from_addr(reinterpret_cast<MAddress>(object));
        IterateVerifyFields(object, [&](RefField<>& field) { closure.do_oop(&field); });
    });
}
void ZVerify::AfterRelocationInternal(ZForwarding* forwarding)
{
    ZVerifyRemsetAfterOopClosure closure(forwarding);
    forwarding->for_each_from([&](MAddress from) {
        ZGeneration* generation = forwarding->from_age() == PageAge::old
            ? static_cast<ZGeneration*>(ZGeneration::old()) : static_cast<ZGeneration*>(ZGeneration::young());
        BaseObject* object = generation->remap_object(reinterpret_cast<BaseObject*>(from));
        closure.set_from_addr(from);
        closure.set_to_addr(reinterpret_cast<MAddress>(object));
        IterateVerifyFields(object, [&](RefField<>& field) { closure.do_oop(&field); });
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
