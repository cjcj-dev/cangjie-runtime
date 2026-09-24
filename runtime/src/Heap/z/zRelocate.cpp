// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zAbort.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include <array>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include "Concurrency/Concurrency.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "Heap/z/zStackWatermark.hpp"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include <cmath>
#include <sched.h>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif
#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"
#include <chrono>
#include "Heap/z/zPage.hpp"
#include "Base/Log.h"
#include "Heap/z/zGeneration.hpp"




namespace MapleRuntime {

// ZGC zRelocate.cpp:1051-1078: claim each thread once across the workers.
class ZRelocateStoreBufferInstallBasePointersThreadClosure {
public:
    void do_thread(Mutator& mutator)
    {
        mutator.GetGCData().storeBarrierBuffer->install_base_pointers();
    }
};

class ZRelocateStoreBufferInstallBasePointersTask final : public ZTask {
    JavaThreadsIterator threads;
public:
    explicit ZRelocateStoreBufferInstallBasePointersTask(ZGeneration* generation)
        : ZTask("ZRelocateStoreBufferInstallBasePointersTask"), threads(generation->id_optional()) {}

    void work() override
    {
        ZRelocateStoreBufferInstallBasePointersThreadClosure closure;
        threads.Apply([&](Mutator& mutator) { closure.do_thread(mutator); });
    }
};

// ZGC zRelocate.cpp:1289: both generations submit their installed set.
void ZRelocate::relocate(ZRelocationSet* relocation_set)
{
    CHECK(relocation_set->generation() == generation);
    auto& manager = Heap::GetHeap().page_allocator();
    ZWorkers& workers = *generation->Workers();
    {
        // ZGC zRelocate.cpp:1289-1296: preserve object starts before page
        // relocation destroys the liveness information used to find them.
        ZRelocateStoreBufferInstallBasePointersTask bufferTask(generation);
        workers.run(&bufferTask);
    }
    if (generation->is_young()) {
        ForwardTask<Generation::Young> task(manager, relocation_set);
        workers.run(&task);
    } else {
        ForwardTask<Generation::Old> task(manager, relocation_set);
        workers.run(&task);
    }
}

bool ZRelocate::IsFromObject(BaseObject* obj)
    {
        if (!Heap::IsHeapAddress(obj)) {
            return false;
        }
        const MAddress addr = reinterpret_cast<MAddress>(obj);
        return Heap::GetHeap().GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
               Heap::GetHeap().GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr;
    }

void ZRelocate::StartRelocationTasks(ZGenerationId generation)
{
    ZWorkers& workers = *Heap::GetHeap().GetZGeneration(generation).Workers();
    auto& queue = *Heap::GetHeap().GetZGeneration(generation).relocate().queue();
    CHECK(!queue.IsActive());
    queue.BeginWorkers(workers.active_workers());
}

// ZGC zRelocate.cpp:733-740.
static bool AddRemsetIfYoung(volatile zpointer* field, zaddress address)
{
    if (Heap::page(untype(address))->IsYoungRegion()) {
        Heap::page(reinterpret_cast<MAddress>(field))->remember(field);
        return true;
    }
    return false;
}

// ZGC zRelocate.cpp:742-794: defer unresolved young relocation; eagerly
// remap null and old targets so they do not need a remembered-set entry.
static void UpdateRemsetPromotedFilterAndRemapPerField(RefField<>& field)
{
    volatile zpointer* const p = reinterpret_cast<volatile zpointer*>(&field);
    const zpointer ptr = field.GetFieldValue();
    CHECK_DETAIL(ZPointer::is_old_load_good(ptr), "promoted field must be old load-good");
    if (ZPointer::is_store_good(ptr)) {
        return;
    }
    if (ZPointer::is_load_good(ptr)) {
        if (!is_null_any(ptr)) {
            AddRemsetIfYoung(p, ZPointer::uncolor(ptr));
        }
        return;
    }
    if (is_null_any(ptr)) {
        ZBarrier::remap_young_relocated(p, ptr);
        return;
    }
    const zaddress_unsafe address = ZPointer::uncolor_unsafe(ptr);
    ZForwarding* const forwarding = generation_forwarding_table(Generation::Young).get(untype(address));
    if (forwarding == nullptr) {
        if (!AddRemsetIfYoung(p, safe(address))) {
            ZBarrier::remap_young_relocated(p, ptr);
        }
        return;
    }
    const MAddress to = forwarding->find(untype(address));
    if (to != 0) {
        if (!AddRemsetIfYoung(p, to_zaddress(to))) {
            ZBarrier::remap_young_relocated(p, ptr);
        }
        return;
    }
    Heap::page(reinterpret_cast<MAddress>(p))->remember(p);
}

void RegionManager::RememberPromotedObject(BaseObject* object)
{
    ZIterator::basic_oop_iterate(object, UpdateRemsetPromotedFilterAndRemapPerField);
}

// ZGC zRelocate.cpp:1227-1255.
static void RemapAndMaybeAddRemset(RefField<>& field)
{
    volatile zpointer* const p = reinterpret_cast<volatile zpointer*>(&field);
    const zpointer ptr = field.GetFieldValue();
    if (ZPointer::is_store_good(ptr)) {
        return;
    }
    const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(p, ptr);
    if (is_null(address)) {
        return;
    }
    if (Heap::is_old(untype(address))) {
        return;
    }
    Heap::page(reinterpret_cast<MAddress>(p))->remember(p);
}

void RegionManager::RememberFlipPromotedPages(ZWorkers& workers)
{
    ZArray<ZPage*>* pages = Heap::GetHeap().GetZGeneration(ZGenerationId::young)
                                .relocation_set().flip_promoted_pages();
    class PageTask final : public ZRestartableTask {
    public:
        explicit PageTask(ZArray<ZPage*>* pages)
            : ZRestartableTask("ZRelocateAddRemsetForFlipPromoted"), iter(pages) {}
        void work() override
        {
            SuspendibleThreadSetJoiner stsJoiner;
            for (ZPage* page; iter.next(&page);) {
                page->object_iterate([&](BaseObject* object) {
                    ZIterator::basic_oop_iterate_safe(object, object->GetTypeInfo(), RemapAndMaybeAddRemset);
                });
                SuspendibleThreadSet::yield();
                if (ZGeneration::young()->Workers()->should_worker_resize()) {
                    return;
                }
            }
        }
    private:
        ZArrayParallelIterator<ZPage*> iter;
    } task(pages);
    workers.run(&task);
}

void ZRelocate::UpdateRemsetOldToOld(ZForwarding* forwarding, BaseObject* from, BaseObject* to)
{
    // ZGC zRelocate.cpp:652-738: the forwarding retains the source identity;
    // the young sequence selects the face active when old relocation started.
    ZPage* const fromPage = forwarding->page();
    ZPage* const toPage = Heap::page(reinterpret_cast<MAddress>(to));
    const uintptr_t fromLocal = fromPage->local_offset(reinterpret_cast<MAddress>(from));
    const size_t size = RegionSpace::GetAllocSize(*to);
    const bool iterateCurrent = Heap::GetHeap().OldActiveRemsetIsCurrent() && !forwarding->in_place();
    auto iter = iterateCurrent
        ? fromPage->remset_iterator_limited_current(fromLocal, size)
        : fromPage->remset_iterator_limited_previous(fromLocal, size);
    BitMap::idx_t index;
    while (iter.next(&index)) {
        const uintptr_t offset = ZRememberedSet::to_offset(index) - fromLocal;
        const MAddress field = reinterpret_cast<MAddress>(to) + offset;
        if (ZGeneration::young()->is_phase_mark()) {
            forwarding->relocated_remembered_fields_register(field);
        } else {
            toPage->remember(reinterpret_cast<volatile zpointer*>(field));
        }
    }
}

void ZRelocate::UpdateRemsetForFields(ZForwarding* forwarding, BaseObject* from, BaseObject* to)
{
    // ZGC zRelocate.cpp:801-815: use the immutable relocation plan, including
    // when an in-place destination has already replaced the source page table entry.
    if (forwarding->to_age() != PageAge::old) {
        return;
    }
    if (forwarding->from_age() == PageAge::old) {
        UpdateRemsetOldToOld(forwarding, from, to);
        return;
    }
    RegionManager::RememberPromotedObject(to);
}

BaseObject* ZRelocate::relocate_object_inner(ZForwarding* forwarding, BaseObject* obj)
{
    // ZGC zRelocate.cpp:354-380: allocation, disjoint copy, insert, undo.
    assert(Heap::GetHeap().IsSurvivedObject(obj) && "Should be live");
    const size_t size = RegionSpace::GetAllocSize(*obj);
    const PageAge toAge = forwarding->to_age();
    BaseObject* toObj = reinterpret_cast<BaseObject*>(
        Heap::GetHeap().object_allocator().alloc_for_relocation(size, toAge));
    if (toObj == nullptr) {
        return nullptr;
    }

    ZUtils::object_copy_disjoint(to_zaddress(reinterpret_cast<uintptr_t>(obj)),
                                 to_zaddress(reinterpret_cast<uintptr_t>(toObj)), size);
    // Cangjie has no return statepoint; the new copy needs a normal header.
    toObj->SetStateCode(ObjectState::NORMAL);
    BaseObject* result = reinterpret_cast<BaseObject*>(forwarding->insert(
        reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(toObj)));
    if (result != toObj) {
        Heap::GetHeap().undo_alloc_object_for_relocation(reinterpret_cast<MAddress>(toObj), size);
    }
    return result;
}


} // namespace MapleRuntime

namespace MapleRuntime {
// ZGC zRelocate.cpp:419-447: target pages are separate from mutator allocation.
static ZPage* AllocateRelocationTarget(ZForwarding* forwarding)
{
    ZAllocationFlags flags;
    flags.set_non_blocking();
    flags.set_gc_relocation();
    ZPage* source = forwarding->page();
    return Heap::alloc_page(forwarding->size(), source->type(), false, forwarding->to_age(), flags);
}

static void RetireRelocationTarget(ZGeneration* generation, ZPage* page)
{
    if (generation->is_young() && page->age() == PageAge::old) {
        generation->increase_promoted(page->GetRegionAllocatedSize());
    } else {
        generation->increase_compacted(page->GetRegionAllocatedSize());
    }
    if (page->GetRegionAllocatedSize() == 0) { Heap::free_page(page); }
}

ZPage* ZRelocateSmallAllocator::alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target)
{
    ZPage* page = AllocateRelocationTarget(forwarding);
    if (page == nullptr) { inPlaceCount.fetch_add(1, std::memory_order_relaxed); }
    if (target != nullptr) { RetireRelocationTarget(generation, target); }
    return page;
}
void ZRelocateSmallAllocator::free_target_page(ZPage* page)
{
    if (page != nullptr) { RetireRelocationTarget(generation, page); }
}
uintptr_t ZRelocateSmallAllocator::alloc_object(ZPage* page, size_t size) const
{
    return page == nullptr ? 0 : page->alloc_object(size);
}
void ZRelocateSmallAllocator::undo_alloc_object(ZPage* page, uintptr_t addr, size_t size) const
{
    page->undo_alloc_object(addr, size);
}

// ZGC zRelocate.cpp:515-582: a medium in-place page is shared only after
// its source layout and previous remembered bitmap have been consumed.
ZRelocateMediumAllocator::~ZRelocateMediumAllocator()
{
    sharedTargets->apply_and_clear_targets([&](ZPage* page) {
        if (page != nullptr) { RetireRelocationTarget(generation, page); }
    });
}
ZPage* ZRelocateMediumAllocator::alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target)
{
    std::unique_lock<std::mutex> guard(lock);
    changed.wait(guard, [&] { return !inPlace; });
    const uint32_t partition = forwarding->page()->partition_id();
    const PageAge age = forwarding->to_age();
    if (sharedTargets->get(partition, age) == target) {
        ZPage* page = AllocateRelocationTarget(forwarding);
        sharedTargets->set(partition, age, page);
        if (page == nullptr) {
            inPlaceCount.fetch_add(1, std::memory_order_relaxed);
            inPlace = true;
        }
        if (target != nullptr) { RetireRelocationTarget(generation, target); }
    }
    return sharedTargets->get(partition, age);
}
void ZRelocateMediumAllocator::share_target_page(ZPage* page, uint32_t partition)
{
    std::lock_guard<std::mutex> guard(lock);
    CHECK(inPlace && page != nullptr);
    CHECK(sharedTargets->get(partition, page->age()) == nullptr);
    sharedTargets->set(partition, page->age(), page);
    inPlace = false;
    changed.notify_all();
}
uintptr_t ZRelocateMediumAllocator::alloc_object(ZPage* page, size_t size) const
{
    return page == nullptr ? 0 : page->alloc_object_atomic(size);
}
void ZRelocateMediumAllocator::undo_alloc_object(ZPage* page, uintptr_t addr, size_t size) const
{
    page->undo_alloc_object_atomic(addr, size);
}

// ZGC zRelocate.cpp:587-1047. Each worker keeps targets across source pages.
// Only the mutator retain/copy/release path uses ZObjectAllocator.
template<class Allocator>
class ZRelocateWork {
public:
    ZRelocateWork(Allocator* allocator, ZRelocationTargets* targets, ZGeneration* generation)
        : allocator(allocator), targets(targets), generation(generation) {}
    ~ZRelocateWork()
    {
        targets->apply_and_clear_targets([&](ZPage* page) { allocator->free_target_page(page); });
        generation->increase_promoted(otherPromoted);
        generation->increase_compacted(otherCompacted);
    }

    // ZGC zRelocate.cpp:977-985,1031: detach before clearing the old bitmap.
    void clear_remset_before_in_place_reuse(ZPage* page)
    {
        if (forwarding->from_age() != PageAge::old) { return; }
        page->clear_remset_previous();
    }

    void do_forwarding(ZForwarding* owner)
    {
        forwarding = owner;
        ZForwarding::PageWorkScope scope(owner);
        ZPage* page = owner->page();
        ZVerify::BeforeRelocation(owner);
        iterate_objects(page);
        ZVerify::AfterRelocation(owner);
        if (ZVerifyForwarding) { owner->verify(); }
        generation->increase_freed(owner->size());
        const bool inPlace = owner->in_place();
        if (inPlace) { owner->in_place_relocation_finish(); }
        if (owner->from_age() == PageAge::old) { owner->relocated_remembered_fields_after_relocate(); }
        owner->release_page();
        ZPage* source = owner->detach_page();
        if (inPlace) {
            clear_remset_before_in_place_reuse(source);
            const uint32_t partition = source->partition_id();
            ZPage* target = targets->get(partition, owner->to_age());
            target->ResetCensusBoundary();
            allocator->share_target_page(target, partition);
            // ZGC zRelocate.cpp:1026-1037: the in-place page is retained as the
            // relocation target and stays live; route it out of the From role
            // at this completion branch so no later role scan can reclaim it.
            ZPageRole expect = ZPageRole::From;
            (void)source->CASRegionRole(expect, ZPageRole::None);
        } else {
            Heap::free_page(source);
        }
    }

private:
    // ZGC zRelocate.cpp:1001: same single livemap entry as ZPage::object_iterate
    // (zPage.inline.hpp:319-331). The livemap is the only bound; a raw TLAB tail
    // holds no live bit and is never visited.
    void iterate_objects(ZPage* page)
    {
        page->object_iterate([&](BaseObject* object) { relocate_object(object); });
    }
    void increase_other_forwarded(size_t size)
    {
        const size_t aligned = AlignUp<size_t>(size, size_t{1} << forwarding->object_alignment_shift());
        if (forwarding->is_promotion()) { otherPromoted += aligned; }
        else { otherCompacted += aligned; }
    }
    uintptr_t try_relocate_object_inner(BaseObject* object, uint32_t partition)
    {
        const uintptr_t from = reinterpret_cast<uintptr_t>(object);
        const size_t size = object->GetSize();
        ZPage* target = targets->get(partition, forwarding->to_age());
        if (const uintptr_t hit = forwarding->find(from)) {
            increase_other_forwarded(size);
            return hit;
        }
        const uintptr_t addr = allocator->alloc_object(target, size);
        if (addr == 0) { return 0; }
        if (forwarding->in_place() && addr + size > from) {
            ZUtils::object_copy_conjoint(to_zaddress(from), to_zaddress(addr), size);
        } else {
            ZUtils::object_copy_disjoint(to_zaddress(from), to_zaddress(addr), size);
        }
        reinterpret_cast<BaseObject*>(addr)->SetStateCode(ObjectState::NORMAL);
        std::atomic_thread_fence(std::memory_order_release);
        const uintptr_t result = forwarding->insert(from, addr);
        if (result != addr) {
            allocator->undo_alloc_object(target, addr, size);
            increase_other_forwarded(size);
        }
        return result;
    }
    bool try_relocate_object(BaseObject* object, uint32_t partition)
    {
        const uintptr_t result = try_relocate_object_inner(object, partition);
        if (result == 0) { return false; }
        ZRelocate::UpdateRemsetForFields(forwarding, object, reinterpret_cast<BaseObject*>(result));
        return true;
    }
    ZPage* start_in_place_relocation(MAddress watermark)
    {
        forwarding->in_place_relocation_claim_page();
        forwarding->in_place_relocation_start(watermark);
        ZPage* source = forwarding->page();
        ZPage* target = forwarding->is_promotion()
            ? source->clone_for_promotion() : source->reset(forwarding->to_age());
        target->reset_top_for_allocation();
        if (forwarding->from_age() == PageAge::old) {
            if (Heap::GetHeap().OldActiveRemsetIsCurrent()) {
                target->verify_remset_cleared_previous();
                source->swap_remset_bitmaps();
            } else {
                target->verify_remset_cleared_current();
            }
        }
        if (forwarding->is_promotion()) {
            ZGeneration::young()->in_place_relocate_promote(source, target);
            ZGeneration::young()->register_in_place_relocate_promoted(source);
        }
        return target;
    }
    void relocate_object(BaseObject* object)
    {
        // ZGC zRelocate.cpp:902-905: the worker checks at the outer entry.
        assert(Heap::GetHeap().IsSurvivedObject(object) && "Should be live");
        const uint32_t partition = forwarding->page()->partition_id();
        const PageAge age = forwarding->to_age();
        while (!try_relocate_object(object, partition)) {
            ZPage* target = targets->get(partition, age);
            ZPage* page = allocator->alloc_and_retire_target_page(forwarding, target);
            targets->set(partition, age, page);
            if (page != nullptr) { continue; }
            page = start_in_place_relocation(reinterpret_cast<MAddress>(object));
            targets->set(partition, age, page);
        }
    }
    Allocator* allocator;
    ZRelocationTargets* targets;
    ZGeneration* generation;
    ZForwarding* forwarding{nullptr};
    size_t otherPromoted{0};
    size_t otherCompacted{0};
};

template<Generation G>
void ForwardTask<G>::work()
{
    ZGeneration* generation = relocationSet->generation();
    ZRelocate& relocate = generation->relocate();
    ZRelocateWork<ZRelocateSmallAllocator> small(&smallAllocator, relocate.small_targets()->addr(), generation);
    ZRelocateWork<ZRelocateMediumAllocator> medium(&mediumAllocator, relocate.medium_targets()->addr(), generation);
    ZRelocateQueue& queue = *relocate.queue();
    const auto doForwarding = [&](ZForwarding* owner) {
        if (owner->page()->is_small()) { small.do_forwarding(owner); }
        else { medium.do_forwarding(owner); }
        owner->mark_done();
    };
    for (;;) {
        for (ZForwarding* owner; (owner = queue.synchronize_poll()) != nullptr;) { doForwarding(owner); }
        ZForwarding* owner = nullptr;
        if (!iter.next(&owner)) { break; }
        if (owner->claim()) { doForwarding(owner); }
        // ZGC zRelocate.cpp:1206-1214: finish one ordinary forwarding before
        // yielding the worker. The shared iterator survives the restart.
        if (generation->should_worker_resize()) { break; }
    }
    queue.leave();
}
template class ForwardTask<Generation::Young>;
template class ForwardTask<Generation::Old>;

} // namespace MapleRuntime

namespace MapleRuntime {


bool ZRelocateQueue::needs_attention() const
{
    return needsAttention.load(std::memory_order_relaxed) != 0;
}

void ZRelocateQueue::inc_needs_attention()
{
    needsAttention.fetch_add(1, std::memory_order_acq_rel);
}

void ZRelocateQueue::dec_needs_attention()
{
    needsAttention.fetch_sub(1, std::memory_order_acq_rel);
}

void ZRelocateQueue::activate(uint32_t workers)
{
    isActive.store(true, std::memory_order_release);
    join(workers);
}

void ZRelocateQueue::deactivate()
{
    isActive.store(false, std::memory_order_release);
    clear();
}

bool ZRelocateQueue::is_active() const
{
    return isActive.load(std::memory_order_acquire);
}

void ZRelocateQueue::join(uint32_t workers)
{
    CHECK_DETAIL(workers != 0 && nworkers == 0 && nsynchronized == 0,
                 "invalid relocate queue join workers=%u nworkers=%u nsync=%u",
                 workers, nworkers, nsynchronized);
    nworkers = workers;
}

void ZRelocateQueue::resize_workers(uint32_t workers)
{
    CHECK_DETAIL(workers != 0 && nworkers == 0 && nsynchronized == 0,
                 "invalid relocate queue resize workers=%u nworkers=%u nsync=%u",
                 workers, nworkers, nsynchronized);
    std::lock_guard<std::mutex> guard(lock);
    nworkers = workers;
}

void ZRelocateQueue::leave()
{
    std::lock_guard<std::mutex> guard(lock);
    nworkers--;
    const bool done = prune();
    const bool last = synchronizeFlag && nworkers == nsynchronized;
    if (done || last) {
        attention.notify_all();
    }
}

void ZRelocateQueue::add_and_wait(ZForwarding* forwarding)
{
    std::unique_lock<std::mutex> guard(lock);
    if (forwarding->is_done()) {
        return;
    }
    queue.append(forwarding);
    if (queue.length() == 1) {
        inc_needs_attention();
        attention.notify_all();
    }
    while (!forwarding->is_done()) {
        attention.wait(guard);
    }
}

bool ZRelocateQueue::prune()
{
    if (queue.is_empty()) {
        return false;
    }
    bool done = false;
    for (int i = 0; i < queue.length();) {
        ZForwarding* forwarding = queue.at(i);
        if (forwarding->is_done()) {
            done = true;
            queue.delete_at(i);
            completionCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            i++;
        }
    }
    if (queue.is_empty()) {
        dec_needs_attention();
    }
    return done;
}

ZForwarding* ZRelocateQueue::prune_and_claim()
{
    if (prune()) {
        attention.notify_all();
    }
    for (int i = 0; i < queue.length(); i++) {
        ZForwarding* forwarding = queue.at(i);
        if (forwarding->claim()) {
            return forwarding;
        }
    }
    return nullptr;
}

void ZRelocateQueue::synchronize_thread()
{
    nsynchronized++;
    if (nsynchronized == nworkers) {
        attention.notify_all();
    }
}

void ZRelocateQueue::desynchronize_thread()
{
    nsynchronized--;
}

ZForwarding* ZRelocateQueue::synchronize_poll()
{
    if (!needs_attention()) {
        return nullptr;
    }
    std::unique_lock<std::mutex> guard(lock);
    if (ZForwarding* forwarding = prune_and_claim()) {
        return forwarding;
    }
    if (!synchronizeFlag) {
        return nullptr;
    }
    synchronize_thread();
    do {
        attention.wait(guard);
        if (ZForwarding* forwarding = prune_and_claim()) {
            desynchronize_thread();
            return forwarding;
        }
    } while (synchronizeFlag);
    desynchronize_thread();
    return nullptr;
}

void ZRelocateQueue::clear()
{
    if (queue.is_empty()) {
        return;
    }
    queue.clear();
    dec_needs_attention();
}

void ZRelocateQueue::synchronize()
{
    std::unique_lock<std::mutex> guard(lock);
    synchronizeFlag = true;
    inc_needs_attention();
    while (nworkers != nsynchronized) {
        attention.wait(guard);
    }
}

void ZRelocateQueue::desynchronize()
{
    std::lock_guard<std::mutex> guard(lock);
    synchronizeFlag = false;
    dec_needs_attention();
    attention.notify_all();
}

ZRelocateQueue::EnqueueResult ZRelocateQueue::Add(void* region, MAddress from)
{
    auto owner = forwarding_for_page(static_cast<ZPage*>(region));
    CHECK_DETAIL(!owner || owner->covers(from), "relocation request outside forwarding from=%#zx", from);
    return Add(owner);
}

ZRelocateQueue::EnqueueResult ZRelocateQueue::Add(ZForwarding* forwarding)
{
    if (forwarding == nullptr) {
        return { nullptr, false, false, nullptr };
    }
    std::lock_guard<std::mutex> guard(lock);
    if (forwarding->is_done()) {
        return { forwarding, false, true, forwarding };
    }
    if (!isActive.load(std::memory_order_acquire) && !forwarding->claimed().load(std::memory_order_acquire)) {
        return { nullptr, false, false, nullptr };
    }
    for (int i = 0; i < queue.length(); i++) {
        if (queue.at(i) == forwarding) {
            return { forwarding, false, true, forwarding };
        }
    }
    queue.append(forwarding);
    if (queue.length() == 1) {
        inc_needs_attention();
    }
    attention.notify_all();
    return { forwarding, true, true, forwarding };
}

void ZRelocateQueue::Wait(ZForwarding* forwarding)
{
    add_and_wait(forwarding);
}

size_t ZRelocateQueue::Complete(ZForwarding* forwarding)
{
    std::lock_guard<std::mutex> guard(lock);
    (void)forwarding;
    const bool done = prune();
    attention.notify_all();
    return done ? 1 : 0;
}

ZForwarding* ZRelocateQueue::PruneAndClaim()
{
    std::lock_guard<std::mutex> guard(lock);
    return prune_and_claim();
}

ZRelocateQueue::Selection ZRelocateQueue::SynchronizePoll()
{
    std::unique_lock<std::mutex> guard(lock);
    if (ZForwarding* forwarding = prune_and_claim()) {
        return { forwarding, nullptr, false };
    }
    CHECK_DETAIL(nworkers != 0 && nsynchronized < nworkers,
                 "invalid relocation worker synchronization workers=%u synchronized=%u",
                 nworkers, nsynchronized);
    ++nsynchronized;
    if (nsynchronized == nworkers) {
        isActive.store(false, std::memory_order_release);
        nworkers = 0;
        nsynchronized = 0;
        attention.notify_all();
        return { nullptr, nullptr, true };
    }
    for (;;) {
        attention.wait(guard);
        if (!isActive.load(std::memory_order_acquire)) {
            return { nullptr, nullptr, true };
        }
        if (ZForwarding* forwarding = prune_and_claim()) {
            --nsynchronized;
            return { forwarding, nullptr, false };
        }
    }
}

size_t ZRelocateQueue::PendingCount() const
{
    std::lock_guard<std::mutex> guard(lock);
    return static_cast<size_t>(queue.length());
}

size_t ZRelocateQueue::SynchronizedWorkerCount() const
{
    std::lock_guard<std::mutex> guard(lock);
    return nsynchronized;
}

PageAge ZRelocate::compute_to_age(PageAge fromAge)
{
    const uint32_t threshold = ZGeneration::young()->tenuring_threshold();
    return ComputeToAge(fromAge, threshold);
}

void ZRelocate::flip_age_pages(ZWorkers& workers, const ZArray<ZPage*>* pages)
{
    class ZFlipAgePagesTask : public ZTask {
    public:
        explicit ZFlipAgePagesTask(const ZArray<ZPage*>* pages)
            : ZTask("ZFlipAgePagesTask"), iter(pages)
        {}
        void work() override
        {
            SuspendibleThreadSetJoiner stsJoiner;
            ZArray<ZPage*> promoted;
            for (ZPage* prev; iter.next(&prev);) {
                const PageAge fromAge = prev->age();
                const PageAge toAge = ZRelocate::compute_to_age(fromAge);
                const bool promotion = toAge == PageAge::old;
                ZPage* const newPage = promotion
                    ? prev->clone_for_promotion()
                    : prev->reset(toAge);
                newPage->reset_livemap();
                if (promotion) {
                    // After the flip the from_page is referenced only by the
                    // relocation set's _flip_promoted_pages (zRelocate.cpp:1355-1363
                    // pushes prev_page; zRelocationSet.cpp:208 asserts no
                    // duplicates). Its lifecycle role is handed to
                    // newPage here, at the single promotion fork, before
                    // flip_promote; the role transfer retains the same byte
                    // charge, so the used/census readers see no
                    // change. flip_promote itself does no list work
                    // (zGeneration.cpp:941-948).
                    // #710: the page lifecycle role (not a list slot) is handed
                    // to newPage at the single promotion fork, before flip_promote.
                    const ZPageRole role = prev->GetRegionRole();
                    prev->SetRegionRole(ZPageRole::None);
                    newPage->SetRegionRole(role);
                    ZGeneration::young()->flip_promote(prev, newPage);
                    promoted.append(prev);
                }
                SuspendibleThreadSet::yield();
            }
            // zRelocate.cpp:1363: registration goes through the generation.
            ZGeneration::young()->register_flip_promoted(promoted);
        }
    private:
        ZArrayParallelIterator<ZPage*> iter;
    };
    ZFlipAgePagesTask task(pages);
    workers.run(&task);
}

void ZRelocate::barrier_promoted_pages(ZWorkers& workers, const ZArray<ZPage*>* flipPromoted,
                                       const ZArray<ZPage*>* relocatePromoted)
{
    class ZPromoteBarrierTask : public ZTask {
    public:
        ZPromoteBarrierTask(const ZArray<ZPage*>* flip, const ZArray<ZPage*>* relocate)
            : ZTask("ZPromoteBarrierTask"), flipIter(flip), relocateIter(relocate)
        {}
        void work() override
        {
            SuspendibleThreadSetJoiner stsJoiner;
            auto promoteBarriers = [](ZArrayParallelIterator<ZPage*>* iter) {
                for (ZPage* page; iter->next(&page);) {
                    page->object_iterate([](BaseObject* obj) {
                        ZIterator::basic_oop_iterate_safe(obj, [](RefField<>& field) {
                            ZBarrier::promote_barrier_on_young_oop_field(
                                reinterpret_cast<volatile zpointer*>(&field));
                        });
                    });
                    SuspendibleThreadSet::yield();
                }
            };
            promoteBarriers(&flipIter);
            promoteBarriers(&relocateIter);
        }
    private:
        ZArrayParallelIterator<ZPage*> flipIter;
        ZArrayParallelIterator<ZPage*> relocateIter;
    };
    ZPromoteBarrierTask task(flipPromoted, relocatePromoted);
    workers.run(&task);
}

} // namespace MapleRuntime

namespace MapleRuntime {
// ZGC zRelocate.cpp:412-416: consume the already published forwarding.
BaseObject* ZRelocate::forward_object(ZForwarding* forwarding, BaseObject* object)
{
    const MAddress to = forwarding->find(reinterpret_cast<MAddress>(object));
    DCHECK(to != 0);
    return reinterpret_cast<BaseObject*>(to);
}

// zRelocate.cpp:382-410: lookup, retain/copy/release, then wait/forward.
BaseObject* ZRelocate::relocate_object(ZForwarding* forwarding, BaseObject* object)
{
    const MAddress from = reinterpret_cast<MAddress>(object);
    if (const MAddress to = forwarding->find(from)) {
        return reinterpret_cast<BaseObject*>(to);
    }
    ZPage::RetainScope lease{forwarding};
    if (lease.ok()) {
        DCHECK(generation->is_phase_relocate());
        BaseObject* to = relocate_object_inner(forwarding, object);
        lease.Release();
        if (to != nullptr) {
            return to;
        }
        // ZGC zRelocate.cpp:402-406: only allocation failure after retaining
        // the page requests worker completion here. retain_page itself waits
        // for a claimed page (zForwarding.cpp:95-100).
        relocateQueue.add_and_wait(forwarding);
    }
    return forward_object(forwarding, object);
}
}





namespace MapleRuntime {
// ZGC zRelocate.cpp:1412-1418.
void ZRelocate::synchronize() { relocateQueue.synchronize(); }
void ZRelocate::desynchronize() { relocateQueue.desynchronize(); }
} // namespace MapleRuntime
