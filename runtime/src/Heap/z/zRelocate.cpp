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
#include "Heap/z/zWorkers.hpp"
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"


namespace MapleRuntime {

static const ZStatSubPhase PRemapYoungRoots("RemapYoungRoots", ZGenerationId::old);

// ZGC zRelocate.cpp:1289: both generations submit their installed set.
void ZRelocate::relocate(ZRelocationSet* relocation_set)
{
    CHECK(relocation_set->generation() == generation);
    auto& manager = Heap::GetHeap().page_allocator();
    ZWorkers& workers = *generation->Workers();
    if (!relocateQueue.IsActive()) {
        StartRelocationTasks(generation->id());
    }
    if (generation->is_young()) {
        ForwardTask<Generation::Young> task(manager, relocation_set);
        workers.run(&task);
    } else {
        ForwardTask<Generation::Old> task(manager, relocation_set);
        workers.run(&task);
    }
    const auto inPlace = manager.InPlaceRelocatedCounts();
    generation->StatRelocation()->AtRelocateEnd(inPlace.first, inPlace.second);
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




// installdomain: positive control — how often Resolve/Fix would install a ghost-from that is
// outside GetRoute's liveInfo0 survivor domain. Grant paints that bit before route geometry.








// this api untags current pointer as well as old pointer, caller should take care of this.

// zGeneration.cpp:1470-1527: shared iterators, one worker task, then restore
// the old generation's active worker budget. Native stack/record expansion is
// the Cangjie adapter for the ZGC uncolored-root closure.
class ZRemapYoungRootsTask final : public ZTask {
    ZRemsetTableIterator remset;
    RootsIteratorAllColored colored;
    RootsIteratorAllUncolored uncolored;
public:
    explicit ZRemapYoungRootsTask(unsigned workers)
        : ZTask("ZRemapYoungRootsTask"), remset(&Heap::GetHeap().remembered(), false),
          colored(workers, ZGenerationIdOptional::old), uncolored(ZGenerationIdOptional::old) {}
    void work() override
    {
        colored.Apply([](NativeSlot& root) { (void)ZBarrier::ReadStaticRef(root); });
        uncolored.Apply([] { Runtime::Current().GetConcurrencyModel().VisitGCRoots(); });
        uncolored.ApplyThreads([&](Mutator& mutator) {
        // ZGC ZRemapThreadClosure (zGeneration.cpp:1419-1424): only
        // StackWatermarkSet::finish_processing. Slot heal uses the saved
        // watermark color via ZUncoloredRoot::process. Cangjie still expands
        // headerless records (no return statepoint).
        RootVisitor heapRoots = [&](ObjectRef& root) {
            StackWatermarkProcessOopClosure closure(nullptr, mutator.GetStackWatermark().uncolored_root_color());
            mutator.VisitHeapRootSlots(root, [&](RootSlot& slot) {
                closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
            });
        };
        DerivedPtrVisitor derived = Mutator::MakeDerivedRootVisitor(heapRoots);
        size_t frames = 0;
        (void)StackWatermarkSet::finish_processing(mutator, heapRoots, heapRoots,
                StackWatermark::epoch_id(), &derived, frames);
        });
        Heap::GetHeap().remembered().remap_current(&remset);
    }
};

void ZRelocate::RemapYoungRoots()
{
    ZStatTimerOld timer(PRemapYoungRoots);
    ZWorkers& workers = *Heap::GetHeap().old().Workers();
    const uint32_t previous = workers.active_workers();
    const uint32_t requested = std::min(std::max(Heap::GetHeap().young().Workers()->active_workers() + previous,
                                                    uint32_t{1}), ZOldGCThreads);
    workers.set_active_workers(requested);
    SuspendibleThreadSetJoiner joiner;
    ZRemapYoungRootsTask task(workers.active_workers());
    workers.run(&task);
    workers.set_active_workers(previous);
}

void ZRelocate::StartRelocationTasks(ZGenerationId generation)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = space.GetRegionManager();
    ZWorkers& workers = *Heap::GetHeap().GetZGeneration(generation).Workers();
    auto& queue = *Heap::GetHeap().GetZGeneration(generation).relocate().queue();
    CHECK(!queue.IsActive());
    manager.ResetInPlaceRelocatedCounts();
    queue.BeginWorkers(workers.active_workers());
}

// N2 (MINOR_CONCURRENCY_0805 §八 T-C): CAS-install resolved target under multi-worker fix.
// Same-value concurrent writes converge; first writer wins. Counters for positive control.
namespace {

void EnsureRouteDomainMembership(BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    if (!obj->IsValidObject()) {
        return;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return;
    }
    const bool isGhost = ZRelocate::IsFromObject(obj);
    const bool isFrom = ZRelocate::IsFromObject(obj);
    if (!isGhost && !isFrom) {
        return;
    }
    const zaddress addr = from_object(obj);
    const bool alreadyInDomain = region->is_object_live(addr);
    if (alreadyInDomain) {
        return;
    }
    if (isGhost) {
        // Only paint while FORWARDABLE: relocation freezes liveByteCount.
        if (region->IsForwardingDone() || region->IsRoutingState()) {
            return;
        }
    }
    // Mark the source page livemap.
    (void)ZMark::MarkEntryObject(obj,
        MarkStackEntry(untype(ZAddress::offset(from_object(obj))), true, true, false, false), nullptr);

}

// statresid: force ghost livemap paint while still FORWARDABLE (before any Route
// freezes geometry). Used by the root grant pass and as last-chance before Forward.
// Returns true when AdmitForRoute would accept `obj` after the paint attempt.
bool ForceRootRouteDomainWhileForwardable(BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return false;
    }
    EnsureRouteDomainMembership(obj);
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    if (region == nullptr || !region->IsYoungRegion()) {
        return false;
    }
    // Only paint while FORWARDABLE — after ROUTING/ROUTED/COMPACTED liveByteCount is
    // frozen (S2); late marking would desync Admit from geometry.
    if (region->IsForwardingDone() || region->IsRoutingState()) {
        return region->is_object_live(from_object(obj));
    }
    (void)ZMark::MarkEntryObject(obj,
        MarkStackEntry(untype(ZAddress::offset(from_object(obj))), true, true, false, false), nullptr);
    return region->is_object_live(from_object(obj));
}
} // namespace

// ZBarrier::barrier / remap_young_relocated (zBarrier.inline.hpp:318-361).
// Resolve and heal the same preloaded word. Preserve mark/remember metadata;
// another writer's load-good value terminates the shared self-heal CAS loop.
static BaseObject* RemapPromotedField(RefField<>& field, zpointer observed)
{
    RefField<> value(observed);
    auto loadGood = [](zpointer word) {
        return ZPointer::is_load_good_or_null(to_zpointer(raw(word)));
    };
    if (loadGood(observed)) {
        return to_object(value.GetTargetObject());
    }
    BaseObject* target = to_object(ZBarrier::make_load_good(value.GetFieldValue()));
    CHECK_DETAIL(target != nullptr || !(!is_null_any(to_zpointer(raw(observed)))),
                 "promotion remap must preserve a non-null reference");
    // ZAddress::load_good: upgrade remap bits without claiming a marking epoch.
    const zpointer healed = ZAddress::load_good(from_object(target), observed);
    ZBarrier::self_heal(ZBarrier::is_load_good_or_null_fast_path,
                        reinterpret_cast<volatile zpointer*>(&field), observed, healed, false);
    return target;
}

// ZRelocateWork::update_remset_promoted_filter_and_remap_per_field
// (zRelocate.cpp:741-794). Unfinished young relocation is remembered for
// deferred remapping; a page worker must not wait on another page's work.
void RegionManager::RememberPromotedObject(BaseObject* object)
{
    if (!object->HasRefField()) {
        return;
    }
    // zRelocate.cpp:798: this relocation-work consumer uses the unsafe entry.
    ZIterator::basic_oop_iterate(object, [&](RefField<>& field) {
        const zpointer observed = field.GetFieldValue();
        RefField<> value(observed);
        BaseObject* target = to_object(value.GetTargetObject());
        if (target != nullptr && Heap::IsHeapAddress(target)) {
            const MAddress address = reinterpret_cast<MAddress>(target);
            ZForwarding* forwarding = ZPointer::is_load_good(value.GetFieldValue()) ? nullptr :
                generation_forwarding_table(Generation::Young).get(address);
            const MAddress to = forwarding == nullptr ? address : forwarding->find(address);
            if (to == 0 || Heap::page(to)->IsYoungRegion()) {
                ZPage* holder = Heap::page(reinterpret_cast<MAddress>(&field));
                if (holder != nullptr) {
                    holder->remember(reinterpret_cast<volatile zpointer*>(&field));
                }
                return;
            }
        }
        // Only completed/non-relocating old targets reach eager remapping.
        // Unfinished young forwarding above stays deferred in the remset.
        RemapPromotedField(field, observed);
    });
}

void RegionManager::RememberFlipPromotedPages(ZWorkers& workers)
{
    ZArray<ZPage*>* pages = Heap::GetHeap().GetZGeneration(ZGenerationId::young)
                                .relocation_set().flip_promoted_pages();
    class PageTask final : public ZTask {
    public:
        PageTask(ZArray<ZPage*>* pages, const std::function<void(RefField<>&)>& remember)
            : ZTask("ZRelocateRemsetFlipPromotedPagesTask"), iter(pages), remember(remember) {}
        void work() override
        {
            for (ZPage* page; iter.next(&page);) {
                page->object_iterate([&](BaseObject* object) {
                    RefFieldVisitor remapAndRemember = [&](RefField<>& field) {
                        const zpointer observed = field.GetFieldValue();
                        BaseObject* target = RemapPromotedField(field, observed);
                        if (target != nullptr && Heap::IsHeapAddress(target) &&
                            Heap::page(reinterpret_cast<MAddress>(target))->IsYoungRegion()) {
                            // RegionManager owns access to the remset producer.
                            remember(field);
                        }
                    };
                    // zRelocate.cpp:1275: flip promotion passes the known
                    // klass through the safe entry before field dispatch.
                    ZIterator::basic_oop_iterate_safe(object, object->GetTypeInfo(), remapAndRemember);
                });
            }
        }
    private:
        ZArrayParallelIterator<ZPage*> iter;
        const std::function<void(RefField<>&)> remember;
    } task(pages, [](RefField<>& field) {
        ZPage* holder = Heap::page(reinterpret_cast<MAddress>(&field));
        if (holder != nullptr) {
            holder->remember(reinterpret_cast<volatile zpointer*>(&field));
        }
    });
    workers.run(&task);
}

// permhole receiptization (steer1): RouteObject is geometric (ROUTED before Copy fills
// tip). A tip-valid to is a *receipt* (copy happened). A geometric to with tip==0 is only
//
// Contract of this wait:
//   ① return tip-valid to (receipt), or
//   ② fail the relocation invariant;
//   ③ never return a from address or a null-tip geometric address.
// Distinct from 4e75f2cc: that path is RouteObject *miss* (no plan) on a ghost about to
// be reclaimed — returning from there reinstalls a dying address. Here RouteObject *hit*
// with no tip yet: while still ROUTED/ROUTING, from is not yet CollectRegion'd.
// After object/region publish (FORWARDED|COMPACTED) tip must exist if the plan was real;
// missing tip = permanent hole = invariant violation → CHECK (not hang, not geometric to).
//

// inplaceto: after an in-place compaction the from-layout and the to-layout occupy the *same*
// page span, so the page-scoped ghost-from predicate cannot tell a stale from-address from an
// address that has already been relocated.  ZGC never has to tell them apart: a to-pointer
// carries the remapped colour, its barrier fast path returns before the forwarding table is
// consulted, and zRelocate.cpp:382-389 is therefore only ever entered with a from-address.
// Our root words are plain (no colour), so the discriminator has to be rebuilt from the page's
// own geometry -- the same geometry ZGC records for this exact overlap in
// ZForwarding::in_place_relocation_start (zForwarding.cpp:55-64, _in_place_top_at_start) and
// consumes in ZHeap::is_in (zHeap.cpp:202-208).
//
// The three cases are mutually exclusive and jointly exhaustive for a compacted page whose
// forwarding lookup missed:
//
//   survived(off)              the from-livemap covers this offset, so compact insert
//                              owed a receipt for it and there is none -> refuse.
//   off < allocPtr             the in-place compaction wrote the to-layout over this offset; no
//                              from object is covered here and none ever was, so the address is
//                              a to-address (or an interior of one) and is already current.
//   off >= allocPtr            the abandoned tail above the new top: the from copy is gone and no
//                              to-object was written here -> nothing can be named, refuse.
//
// Measured on NW256/256MB, 3/3 verbatim: a base register root held from-offset 33480 at mark and
// to-offset 27320+2048 at the major PreForward, with the table mapping 33480 onto 27320
// (revBaseHit=1 revBaseFromOff=33480).  The root was current; the walk asked anyway.
// kAlreadyTo is split by what the page's own size walk says the address *is*.  ZGC's heap oop
// fields hold object starts by construction -- interior pointers exist only as derived oops
// paired with a base in an oop map (oopMap.cpp:404-424) and never in a field -- so an interior
// reaching a heap-field consumer is not a to-address that needs recognising, it is a value that
// names nothing.  Only the root-side consumers, where an interior is a legal register value, may
// take kAlreadyToInterior.
enum class CompactedMissClass : uint8_t { kReceiptOwed, kAlreadyToStart, kAlreadyToInterior,
                                          kAbandonedTail };

static CompactedMissClass ClassifyCompactedMiss(ZPage* region, BaseObject* obj)
{
    const MAddress addr = reinterpret_cast<MAddress>(obj);
    const MAddress start = region->GetRegionStart();
    const MAddress allocPtr = region->GetRegionAllocPtr();
    if (addr < start) {
        return CompactedMissClass::kAbandonedTail;
    }
    const size_t off = static_cast<size_t>(addr - start);
    // Inside an in-place compaction the from- and to-layouts share one span, so
    // "the from-livemap covers off" and "off is a published destination" are both true of the
    // same address whenever some from-object landed on top of another from-object's start.  The
    // three cases above are therefore NOT disjoint in that overlap, and asking the livemap first
    // classified a live to-object start as an owed receipt: measured on NW256/256MB, a root at
    // to-offset 18584 with survived=1 isStart=1 whose reverse lookup named from-offset 37256 as
    // the object copied there (revHit=1 revBaseHit=1) was refused as try.compacted-no-receipt.
    // Provenance is a claim only the page's own table can attest, so ask the table before the
    // livemap -- ZGC resolves the identical overlap from ZForwarding::_in_place_top_at_start plus
    // the forwarding entry, never from liveness (zForwarding.cpp:55-64; zHeap.cpp:202-208).
    if (addr < allocPtr) {
        ZForwarding* provenance = forwarding_for_page(region);
        MAddress revFrom = 0;
        if (provenance != nullptr && provenance->find_from_by_to(addr, &revFrom) && revFrom >= start) {
            return CompactedMissClass::kAlreadyToStart;
        }
    }
    if (region->is_object_live(to_zaddress(region->GetRegionStart() + off))) {
        return CompactedMissClass::kReceiptOwed;
    }
    if (addr >= allocPtr) {
        return CompactedMissClass::kAbandonedTail;
    }
    // The to-layout size walk is the discriminator, so it runs before the class is decided, not
    // only for the diagnostic below.  A page whose walk cannot name a container for this address
    // has published nothing that covers it: refuse.
    size_t contOff = 0;
    size_t contSize = 0;
    size_t contDelta = 0;
    unsigned contFound = 0;
    if (region->IsLargeRegion()) {
        contFound = 1;
        contOff = 0;
        contSize = static_cast<size_t>(allocPtr - start);
        contDelta = off;
    } else {
        MAddress position = start;
        for (size_t steps = 0; position < allocPtr && steps < (1u << 20); ++steps) {
            BaseObject* o = reinterpret_cast<BaseObject*>(position);
            const size_t allocSize = RegionSpace::GetAllocSize(*o);
            if (allocSize == 0) {
                break;
            }
            if (addr >= position && addr < position + allocSize) {
                contFound = 1;
                contOff = static_cast<size_t>(position - start);
                contSize = allocSize;
                contDelta = static_cast<size_t>(addr - position);
                break;
            }
            position += allocSize;
        }
    }
    if (contFound == 0) {
        return CompactedMissClass::kAbandonedTail;
    }
    return contDelta == 0 ? CompactedMissClass::kAlreadyToStart
                          : CompactedMissClass::kAlreadyToInterior;
}


// portmutreloc: ZRelocate::relocate_object's retain/copy/release leg (zRelocate.cpp:391-406).
//
// The three pieces map one-to-one onto machinery that already exists here:
//
//   forwarding->retain_page(&_queue)   ->  ZPage::TryLockReadFromRegion()
//   relocate_object_inner(...)         ->  RelocateObjectInner
//   forwarding->release_page()         ->  ZPage::UnlockReadFromRegion()
//
// nullptr means the current thread did not acquire the page. The caller may
// consume a receipt installed by the owning copier, but may not use the from
// address as an alternate result.
BaseObject* ZRelocate::WaitForPageForwarding(BaseObject* obj, ZForwarding* owner) const
{
    if (!owner || ZForwarding::CurrentPageWork() == owner) return nullptr;
    const MAddress from = reinterpret_cast<MAddress>(obj);
    if (const MAddress found = owner->find(from)) {
        return reinterpret_cast<BaseObject*>(found);
    }
    auto& manager = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    if (MutatorManager::Instance().WorldStopped() && !owner->is_done()) {
        // #498: without return barriers roots are completed eagerly. There is
        // no concurrent page worker in this pause; reuse its in-place task.
        ZPage* page = owner->page();
        if (owner->from_age() != PageAge::old) {
            manager.ForwardClaimedPage<Generation::Young>(page, owner, false, true);
        } else {
            manager.ForwardClaimedPage<Generation::Old>(page, owner, false, true);
        }
        if (const MAddress winner = owner->find(from)) return reinterpret_cast<BaseObject*>(winner);
    }
    auto& queue = generation_relocate_queue((owner->from_age() == PageAge::old ? Generation::Old : Generation::Young));
    const auto request = queue.Add(owner);
    CHECK_DETAIL(request.accepted, "relocation request has no page task from=%#zx", from);
    queue.Wait(request.forwarding);
    return reinterpret_cast<BaseObject*>(owner->find(from));
}

bool ZRelocate::IsAlreadyToStoreValue(BaseObject* target, Generation generation)
{
    return target != nullptr && Heap::IsHeapAddress(target) &&
        ZBarrier::JudgeHandOutTarget(target) == HandVerdict::Usable &&
        generation_forwarding_table(generation).get(reinterpret_cast<MAddress>(target)) == nullptr;
}

BaseObject* ZRelocate::ResolveStoreValue(BaseObject* ref,
                                         Generation generation)
{
    // zBarrier.inline.hpp:695-716 store_barrier_on_heap_oop_field:
    // color_store_good includes remap. A movable ghost-from value must go
    // through the same relocate_or_remap funnel as the load barrier
    // (zRelocate.cpp:382-416) before it is painted store-good.
    BaseObject* current = ref;
    for (;;) {
        if (current == nullptr || !Heap::IsHeapAddress(current)) {
            return current;
        }
        if (ZRelocate::IsAlreadyToStoreValue(current, generation)) {
            return current;
        }
        const MAddress currentAddr = reinterpret_cast<MAddress>(current);
        ZPage* currentRegion = Heap::page(currentAddr);
        if (currentRegion != nullptr && currentRegion->IsCompactRouteDestination(currentAddr) &&
            ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable) {
            // Dense in-place destinations share the from page's address range.
            // Their presence in the completed compact route table is the
            // positive relocation receipt; region membership alone must not
            // reinterpret the packed to-address as another from-address.
            return current;
        }
        // inplaceto: the test above pairs a structural question (is this a compact-route
        // destination?) with a content heuristic on the header word, so an *interior* of a
        // relocated object -- whose header word is zero by construction, HandVerdict::ZeroHeader
        // -- can never satisfy it.  Measured, NW256/256MB 3/3: regionStart+4856 refused here with
        // verdict=2, while the table maps from 4872 onto to 4840 and the page layout holds a
        // 48-byte object at 4840 containing it (revBaseHit=1 revBaseFromOff=4872 contDelta=16).
        // The page geometry answers the structural question without reading the payload, which is
        // the order ZGC uses: the forwarding read never depends on the from copy's bytes
        // (zRelocate.cpp:382-389).
        if (currentRegion != nullptr && currentRegion->IsCompacted() &&
            ClassifyCompactedMiss(currentRegion, current) == CompactedMissClass::kAlreadyToStart) {
            // An address-shaped compact destination is not, by itself, a
            // load-good value.  In particular the from header may be a zero
            // header for an interior-shaped probe, so
            // kAlreadyToStart is only a geometric classification.  The
            // resolve postcondition is the same as every other receipt hop:
            // only a Usable object may leave this function.  Keep the
            // non-Usable case on the receipt/relocate path below, which either
            // finds the explicit identity receipt or fails closed.
            if (ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
        }
        const MAddress found = forwarding_find(generation, currentAddr);
        // A forwarding entry qualifies one hop, not necessarily the final
        // load-good value. The destination can already belong to the next
        // relocation set; follow that address-keyed forwarding generation too.
        // ZGC's load barrier returns only after remap/relocate has produced the
        // current address (zBarrier.inline.hpp:294-343; zRelocate.cpp:382-416).
        if (BaseObject* to = reinterpret_cast<BaseObject*>(found)) {
            const HandVerdict verdict = ZBarrier::JudgeHandOutTarget(to);
            if (verdict == HandVerdict::Usable) {
                // from->from is the explicit whole-page in-place receipt
                // (zRelocate.cpp:862-925,1013-1037), not a lookup miss.
                return to;
            }
            if (to != current) {
                current = to;
                continue;
            }
            // Identity with a still-forwarded header is not a hop. Finish
            // relocate_or_remap (zRelocate.cpp:382-416).
        }

        // A missing receipt is not a terminal miss while the from-region is
        // retained: the current thread completes relocation before publishing
        // the healed value (zBarrier.inline.hpp:294-343).
        ZPage* ghost = currentRegion;
        if (ghost == nullptr) {
            ZPage* live = Heap::page(currentAddr);
            if (live != nullptr && live->IsCompacted()) {
                const CompactedMissClass cls = ClassifyCompactedMiss(live, current);
                if (cls == CompactedMissClass::kAlreadyToStart &&
                    ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                    return current;
                }
            }
            if (live != nullptr && !live->IsFreeRegion() && !live->IsGarbageRegion() &&
                ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
            const MAddress lookupTo = forwarding_find(generation, currentAddr);
            LOG(RTLOG_ERROR,
                "[FWDTABLE][resolve-miss] site=no-forwarding consumer=ZRelocate::ResolveStoreValue "
                "from=%p from_region=%p region_type=%u generation=%u "
                "in_current_relocation_set=%u table_id=%#zx lookup_state=%u "
                "from_page_epoch=%llu lifeId=%llu "
                "gc_phase=%u ghost=0 compacted=%u route=%u lookup.to=%p "
                " verdict=%u",
                static_cast<void*>(current), static_cast<void*>(live),
                live != nullptr ? 0u : 0xffu,
                live != nullptr ? static_cast<unsigned>(live->generation_id()) : 0xffu,
                (currentAddr != 0 && generation_forwarding_table(generation).get(currentAddr) != nullptr) ? 1u : 0u, 0zu,
                0u,
                0ull,
                0ull,
                ZGeneration::old() != nullptr ? static_cast<unsigned>(ZGeneration::old()->Snapshot().phase) : 0xffu,
                live != nullptr && live->IsCompacted() ? 1u : 0u,
                live != nullptr ? live->RelocateObserve() : 0u,
                reinterpret_cast<void*>(lookupTo),
                static_cast<unsigned>(ZBarrier::JudgeHandOutTarget(current)));
            ZBarrier::FailClosedLoad("ZRelocate::ResolveStoreValue.no-forwarding", current, 0);
        }
        // A pointer with ghost membership belongs to a published forwarding
        // generation. Even after its route state changes it cannot be
        // reclassified as a non-member; only an explicit receipt or completed
        // relocation qualifies a value (zRelocate.cpp:408-415).
        BaseObject* resolved = ZGeneration::generation(static_cast<ZGenerationId>(generation))->relocate_or_remap_object(current);
        if (resolved == nullptr) {
            ZBarrier::FailClosedLoad("ZRelocate::ResolveStoreValue.unresolved", current, 0);
        }
        if (resolved == current) {
            // In-place completion must have published its identity receipt;
            // without it, returning current would recreate the removed
            // lookup-miss fallback.
            const MAddress identity = forwarding_find(generation, currentAddr);
            if (identity == currentAddr &&
                ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable) {
                return current;
            }
            // zGeneration.inline.hpp:131-135: forwarding table gone → safe(addr).
            // Ghost can be dispelled between the membership check and
            // relocate_or_remap; that is not a missing identity receipt.
            if (ZBarrier::JudgeHandOutTarget(current) == HandVerdict::Usable &&
                Heap::page(currentAddr) == nullptr) {
                return current;
            }
            ZBarrier::FailClosedLoad("ZRelocate::ResolveStoreValue.missing-identity", current, 0);
        }
        current = resolved;
    }
}

BaseObject* ZRelocate::ForwardObject(BaseObject* obj, Generation generation)
{
    BaseObject* to = ZGeneration::generation(static_cast<ZGenerationId>(generation))->relocate_or_remap_object(obj);
    if (to != nullptr && to != obj) {
        return to;
    }
    // GetRoute survivor gate / exclusive soft-miss: a movable ghost-from with no
    // to-version is not a stable address. Returning `obj` here reinstalls a from
    // pointer that CollectRegion is about to reclaim → UAF / HANG under ALOT.
    // Unmovable / non-ghost still keep `obj` (in-place / not in route domain).
    if (IsFromObject(obj)) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
        BaseObject* waited = ZGeneration::generation(static_cast<ZGenerationId>(generation))->relocate()
            .WaitForPageForwarding(obj, forwarding_for_page(region));
        if (waited != nullptr) {
            return waited;
        }
        if (const MAddress hit = forwarding_find(generation, reinterpret_cast<MAddress>(obj))) {
            return reinterpret_cast<BaseObject*>(hit);
        }
        // zRelocate.cpp:412-415: after wait, the table holds the winner. The page
        // worker copying this object (CurrentPageWork) must not wait on itself.
        if (ZForwarding::CurrentPageWork() != nullptr) {
            return nullptr;
        }
        CHECK_DETAIL(false, "should be forwarded from=%p", obj);
        return nullptr;
    }
    return obj;
}

BaseObject* ZRelocate::ForwardObjectExclusive(BaseObject* obj)
{
    ZPage* page = Heap::page(reinterpret_cast<MAddress>(obj));
    if (page == nullptr) {
        page = Heap::page(reinterpret_cast<MAddress>(obj));
    }
    if (page == nullptr) {
        return nullptr;
    }
    return ZGeneration::generation(page->generation_id())->relocate().relocate_object_inner(obj, page);
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

BaseObject* ZRelocate::relocate_object_inner(BaseObject* obj, ZPage* copyPage)
{
    const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
    if (const MAddress hit = (forwarding_for_page(copyPage) != nullptr ? forwarding_for_page(copyPage)->find(fromAddr) : 0)) {
        BaseObject* to = reinterpret_cast<BaseObject*>(hit);
        UpdateRemsetForFields(forwarding_for_page(copyPage), obj, to);
        return to;
    }
    const size_t size = RegionSpace::GetAllocSize(*obj);
    // ZObjectAllocator::alloc_for_relocation: per-age shared allocation, non-blocking.
    const PageAge toAge = forwarding_for_page(copyPage)->to_age();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    BaseObject* toObj = reinterpret_cast<BaseObject*>(Heap::GetHeap().object_allocator().alloc(size, toAge, true));
    if (toObj == nullptr) return nullptr;
    BaseObject* result = nullptr;
    ZForwarding* publication = forwarding_for_page(copyPage);
    if (publication != nullptr) {
        DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
        // zRelocate.cpp:369: a fresh to-page copy is disjoint.
        ZUtils::object_copy_disjoint(to_zaddress(reinterpret_cast<uintptr_t>(obj)),
                                     to_zaddress(reinterpret_cast<uintptr_t>(toObj)), size);
        if (toObj != obj) {
            toObj->SetStateCode(ObjectState::NORMAL);
        }
        std::atomic_thread_fence(std::memory_order_release);
        if (toObj == obj || (toObj != nullptr)) {
            const MAddress mapped = publication->insert(reinterpret_cast<MAddress>(obj), reinterpret_cast<MAddress>(toObj));
            if (mapped != 0) {
                // ZGC keeps no FORWARDED header state: the forwarding table is
                // the only truth (zRelocate.cpp:382-415; zForwarding has no
                // header bit). The insert above is the whole publication.
                result = reinterpret_cast<BaseObject*>(mapped);
            }
        }
    } else {
        const MAddress pageStart = copyPage == nullptr ? 0 : copyPage->GetRegionStart();
        const uint64_t entries = forwarding_for_page(copyPage) == nullptr ? 0 : 1;
        LOG(RTLOG_ERROR,
            "[GCV2][first-visitor] publication refused obj=%p page=%p pageStart=%#zx entries=%llu route=%u done=%u ref=%d",
            obj, copyPage, static_cast<size_t>(pageStart), static_cast<unsigned long long>(entries),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->RelocateObserve()),
            copyPage == nullptr ? 0U : static_cast<unsigned>(copyPage->IsForwardingDone()),
            copyPage == nullptr ? 0 : copyPage->ForwardingRefCount());
    }
    if (result != toObj) {
        if (ZPage* dest = Heap::page(reinterpret_cast<MAddress>(toObj))) {
            (void)dest->undo_alloc_object_atomic(reinterpret_cast<uintptr_t>(toObj), size);
        }
    }
    if (result != nullptr) {
        UpdateRemsetForFields(publication, obj, result);
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
    ZPage* page = Heap::alloc_page(forwarding->size(), source->type(), false, false,
                                  true, forwarding->to_age(), flags);
    if (page == nullptr) {
        Heap::GetHeap().page_allocator().NoteInPlaceRelocated(source);
    }
    return page;
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
        if (page == nullptr) { inPlace = true; }
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

    // The stack-root eager completion entry has no return statepoint in
    // Cangjie. It uses this same in-place setup and object loop.
    void compact(ZForwarding* owner)
    {
        forwarding = owner;
        ZForwarding::PageWorkScope scope(owner, ZForwarding::CurrentPageWork() != owner);
        ZPage* page = owner->page();
        const MAddress start = page->GetRegionStart();
        ZPage* target = start_in_place_relocation(start);
        targets->set(page->partition_id(), owner->to_age(), target);
        iterate_objects(page);
        target->ResetCensusBoundary();
        owner->in_place_relocation_finish();
        page->MarkForwardingDone();
        // ZGC zRelocate.cpp:1026-1037: the in-place page is retained as the
        // relocation target and stays live; route it out of the From role at
        // this completion branch so no later role scan can reclaim it.
        ZPageRole expect = ZPageRole::From;
        (void)page->CASRegionRole(expect, ZPageRole::None);
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
        if (forwarding->ref_count().load(std::memory_order_acquire) > 0) {
            forwarding->in_place_relocation_claim_page();
        }
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
        (void)queue.Complete(owner);
    };
    for (;;) {
        for (ZForwarding* owner; (owner = queue.synchronize_poll()) != nullptr;) { doForwarding(owner); }
        ZForwarding* owner = nullptr;
        if (!iter.next(&owner)) { break; }
        if (owner->claim()) { doForwarding(owner); }
    }
    queue.leave();
}
template class ForwardTask<Generation::Young>;
template class ForwardTask<Generation::Old>;

template<Generation G>
void RegionManager::ForwardClaimedPage(ZPage* region, ZForwarding* owner, bool claimed, bool inPlace)
{
    if (!owner || (!claimed && !owner->claim())) { return; }
    ZForwarding::PageWorkScope pageWork(owner);
    ZGeneration* generation = &Heap::GetHeap().GetZGeneration(
        G == Generation::Young ? ZGenerationId::young : ZGenerationId::old);
    ZRelocationTargets targets;
    ZRelocateSmallAllocator allocator(generation);
    ZRelocateWork<ZRelocateSmallAllocator> work(&allocator, &targets, generation);
    if (inPlace) {
        NoteInPlaceRelocated(region);
        work.compact(owner);
        if (owner->from_age() == PageAge::old) { owner->relocated_remembered_fields_after_relocate(); }
        if (owner->ref_count().load(std::memory_order_acquire) != 0) { owner->release_page(); }
        ZPage* source = owner->detach_page();
        work.clear_remset_before_in_place_reuse(source);
    } else {
        work.do_forwarding(owner);
    }
    owner->mark_done();
    (void)generation->relocate().queue()->Complete(owner);
}


namespace {
void WaitCopiedObjectsUnlocked(ZPage* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return;
    }
    ZForwarding::WaitPageDone(forwarding_for_page(region));
}

template<typename Fn>
void ForEachLiveObjectStart(ZPage* region, MAddress start, MAddress allocPtr, Fn&& fn)
{
    // ZPage::object_iterate (zPage.inline.hpp:319-331) over the original page's
    // livemap, retained until in_place_relocation_finish.
    const ZGenerationId id = region->generation_id();
    ZLiveMap* map = &region->livemap();
    if (map == nullptr) {
        return;
    }
    const int shift = region->object_alignment_shift();
    map->iterate(id, [&](BitMap::idx_t index) -> bool {
        const size_t offset = (index / 2) << shift;
        if (start + offset < allocPtr) {
            fn(from_region_addr(start + offset), offset);
        }
        return true;
    });
}

} // namespace

void RegionManager::CompactRegion(ZPage* region)
{
    ZForwarding* owner = forwarding_for_page(region);
    CHECK(owner != nullptr);
    ZGeneration* generation = &Heap::GetHeap().GetZGeneration(
        owner->from_age() == PageAge::old ? ZGenerationId::old : ZGenerationId::young);
    ZRelocationTargets targets;
    ZRelocateSmallAllocator allocator(generation);
    ZRelocateWork<ZRelocateSmallAllocator> work(&allocator, &targets, generation);
    work.compact(owner);
}



namespace {
bool StayYoungThisCycle(ZPage* region)
{
    if (!kPageAgeAdaptiveTenuring) {
        return false;
    }
    const uint32_t thr = ZGeneration::young()->tenuring_threshold();
    return !ShouldPromoteAge(region->GetYoungAge(), thr);
}

} // namespace

void RegionManager::BumpYoungSurvivorAge(ZPage* region)
{
    uint8_t next = region->GetYoungAge();
    if (next < untype(PageAge::survivor14)) {
        region->reset(static_cast<PageAge>(next + 1));
    }
}

void RegionManager::FinishStayYoungInPlace(ZPage* region, bool advanceAge)
{
    if (advanceAge) {
        BumpYoungSurvivorAge(region);
    }
    WaitCopiedObjectsUnlocked(region);
    region->MarkForwardingDone();
    // The selected-set carrier remains queryable after payload release.
    // The next selection/reset retires its ghost/source view; completing this
    // page task does not revoke forwarding-table membership.
}

void RegionManager::EnlistStayYoungSurvivor(ZPage* region, bool advanceAge)
{
    FinishStayYoungInPlace(region, advanceAge);
    // evac_finish calls this on FROM regions. The claim is a role CAS;
    // there is no link chain to corrupt (#710).
    bool claimed = false;
    if (region->IsFromRegion()) {
        ZPageRole expect = ZPageRole::From;
        claimed = region->CASRegionRole(expect, ZPageRole::None);
    } else if (region->IsLoneFromRegion()) {
        claimed = true;
    } else if (region->IsGarbageRegion()) {
        ZPageRole expect = ZPageRole::Garbage;
        claimed = region->CASRegionRole(expect, ZPageRole::None);

    } else if (region->GetRegionRole() == ZPageRole::RecentFull) {
        return;
    }
    if (!claimed) {
        return;
    }
    region->SetRegionRole(ZPageRole::RecentFull);

}

template void RegionManager::ForwardClaimedPage<Generation::Young>(ZPage*, ZForwarding*, bool, bool);
template void RegionManager::ForwardClaimedPage<Generation::Old>(ZPage*, ZForwarding*, bool, bool);

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zJNICritical.hpp"

#include <atomic>
#include <chrono>
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"

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
        attention.wait_for(guard, std::chrono::milliseconds(1));
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
            auto promoteBarriers = [](ZArrayParallelIterator<ZPage*>* iter) {
                for (ZPage* page; iter->next(&page);) {
                    page->object_iterate([](BaseObject* obj) {
                        ZIterator::basic_oop_iterate_safe(obj, [](RefField<>& field) {
                            ZBarrier::promote_barrier_on_young_oop_field(
                                reinterpret_cast<volatile zpointer*>(&field));
                        });
                    });
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
    // Cangjie has no return statepoints: eager root repair also enters through
    // coloured static/export roots before concurrent workers are submitted.
    // Reuse the existing stopped-world page completion adapter (ZGC's ordinary
    // concurrent retain/wait path is zRelocate.cpp:382-410).
    if (MutatorManager::Instance().WorldStopped()) {
        return WaitForPageForwarding(object, forwarding);
    }
    ZPage::RetainScope lease{forwarding};
    if (lease.ok()) {
        DCHECK(generation->is_phase_relocate());
        BaseObject* to = relocate_object_inner(object, forwarding->page());
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

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "Base/Log.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"

namespace MapleRuntime {
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"

#include <array>
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
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zRelocate.hpp"

#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"


namespace MapleRuntime {
}

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"

#include <array>
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
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zRelocate.hpp"

#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"




namespace MapleRuntime {

}

namespace MapleRuntime {
// ZGC zRelocate.cpp:1412-1418.
void ZRelocate::synchronize() { relocateQueue.synchronize(); }
void ZRelocate::desynchronize() { relocateQueue.desynchronize(); }
} // namespace MapleRuntime
