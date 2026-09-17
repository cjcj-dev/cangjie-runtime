// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Heap/z/zAddress.hpp"
#include "Base/TimeUtils.h"
#include "Base/SysCall.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "Allocator/RegionSpace.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Collector/CopyCollector.h"
#include "Heap/z/zMark.hpp"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
class MarkLiveCache;
class ScopedStopTheWorld;

#if defined(MRT_TESTABLE_INTERNALS)
struct Y2yHandoffTestReceipt {
    uint64_t phase0 = 0; // release-boundary observation
    uint64_t phase1 = 0; // roots consumed after release
    uint64_t phase2 = 0; // STW2 merge
    uint64_t beforeRelease = 0;
    uint64_t afterRoot = 0;
    uint64_t afterStw2 = 0;
};
void ResetY2yHandoffTestReceipt();
Y2yHandoffTestReceipt ReadY2yHandoffTestReceipt();
void NoteY2yBeforeReleaseTestReceipt(uint64_t pending);
void NoteY2yAfterRootTestReceipt(uint64_t pending);
void NoteY2yAfterStw2TestReceipt(uint64_t pending);
void ArmY2yAfterReleaseTestReceipt(BaseObject* holder, uint64_t publications);
void PublishY2yAfterReleaseTestReceipt();
void ArmMarkBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second = nullptr);
void PublishMarkBeforeMarkEndTestReceipt();
void ArmY2yDuringConcurrentTestReceipt(BaseObject* holder);
void PublishConcurrentYoungProducersTestReceipt();
void ArmLeftoverBeforePauseTestReceipt(BaseObject* y2yHolder);
void PublishLeftoverBeforePauseTestReceipt();

struct ExportRootPublicationTestReceipt {
    uint64_t registrationsAfterT1 = 0;
    uint64_t producerFlushes = 0;
    uint64_t observedAtT2 = 0;
    U64 handle = std::numeric_limits<U64>::max();
    bool holderMarked = false;
    bool childMarked = false;
};
void ResetExportRootPublicationTestReceipt();
void ArmExportRootAfterT1TestReceipt(Mutator* producer, BaseObject* holder, BaseObject* child);
void PublishExportRootAfterT1TestReceipt();
void FlushExportRootAfterT1TestReceipt();
void NoteExportRootPublicationAtT2TestReceipt();
ExportRootPublicationTestReceipt ReadExportRootPublicationTestReceipt();
#endif

// portyoungconc: work accounting for the concurrent young mark window.
// ZGC anchor: ZGenerationYoung::concurrent_mark() = mark_roots() + mark_follow()
// (zGeneration.cpp:665-669) — everything a young collector does between
// pause_mark_start and pause_mark_end runs with mutators alive.
// Every field below counts GC work performed while the world is running. A
// Duration alone does not establish that the concurrent window performed marking work.
struct YoungConcWindowStats {
    uint64_t windowNs = 0;    // world-released → STW2 requested
    size_t closureCalls = 0;  // TraceYoungClosure invocations inside the window
    size_t markedAtEntry = 0; // reachableVec.size() at world-release
    size_t markedAtExit = 0;  // reachableVec.size() at STW2 request
    size_t remsetSlots = 0;   // remset slots consumed by the in-window rescan
    size_t reenters = 0;      // ZGC pause_mark_end() == false → concurrent_mark_continue()
    size_t MarkedInWindow() const { return markedAtExit >= markedAtEntry ? markedAtExit - markedAtEntry : 0; }
};

#if defined(MRT_TESTABLE_INTERNALS)


#endif

class ForwardTable {
public:
    explicit ForwardTable(RegionSpace& space) : theSpace(space) {}

    // if region is compacted, return false.

    template<Generation G>
    void PrepareForwardTable()
    {
        DLOG(FORWARD, "reset fwd table");
        theSpace.PrepareFromSpace<G>();
    }

    RegionSpace& theSpace;
};

using CrossRefHandler = void(*)(BaseObject*, BaseObject*);

class WCollector : public CopyCollector {
    friend class ZGeneration;
    friend class ZGenerationYoung;
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MutatorPublishTestAccess;
    friend struct PartialArrayTestAccess;
    friend struct RelocationReceiptTestAccess;
    friend struct MarkPort203TestAccess;
    friend struct RemsetRearmTestAccess;
    friend struct LoadHealDeliveryTestAccess;
#endif

#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    explicit WCollector(Allocator& allocator, CollectorResources& resources)
        : CopyCollector(allocator, resources), fwdTable(reinterpret_cast<RegionSpace&>(allocator))
    {
        collectorType = CollectorType::SMOOTH_COLLECTOR;
    }

    ~WCollector() override = default;

#if defined(MRT_GC_UNIT_TESTS)
    void SetCycleRefHandlerForTest(CrossRefHandler handler) { cycleRefHandlerForTest = handler; }

    // Controlled test wrapper for the copier route consumer. It keeps the
    // consumer's preconditions visible (heap address, relocation phase and

#endif

    void MarkNewObject(BaseObject* obj) override;
    void StartYoungMarkWork();
    void DrainAllocBufferMarkProducers(AllocBuffer* buffer, WorkStack& work, bool young);
    bool PublishHandshakeMarkWork(WorkStack& work, ZMark* domain);
    void PublishThreadRoot(BaseObject* object, bool young, bool follow);
    bool FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain);
    bool FlushThreadMarkProducers(ThreadLocalData* tls);
    bool FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain);
    bool FlushGCDataMarkProducers(ThreadGCData& data);
    ZMark* YoungMark() { return youngCycle.MarkPtr(); }
    const ZMark* YoungMark() const { return youngCycle.MarkPtr(); }
    void MarkYoungObjectIfActive(BaseObject* object) const override;
    void MarkYoungRootObject(BaseObject* object) const override;

    bool ShouldIgnoreRequest(GCRequest& request) override;
    bool MarkObject(BaseObject* obj) const override;
    bool ResurrectObject(BaseObject* obj, size_t offset, ZPage* regionInfo) override;

    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void TraceRefField(BaseObject* obj, RefField<>& ref, WorkStack& workStack, bool finalizable = false) const;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack, bool finalizable = false) override;
    void FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack) override;
    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field) override;
    BaseObject* ForwardObject(BaseObject* fromVersion, Generation generation) override;
    BaseObject* ForwardObjectExclusive(BaseObject* obj) override;
    BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
                                  Generation generation) const override;
    void PostResolveCycleTask();
    void PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
        CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
        for (auto& entry : discoveredExternObjects) {
            ValueRootList& destination = cycleRefWorkStack[entry.first];
            destination.splice(destination.end(), entry.second);
        }
        discoveredExternObjects.clear();
    }
    void MergeResurrectExportObjects(Generation generation)
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, generation);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }
    // Phase A of the ZGC-style colouring work (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6).
    //
    // Today a reference carries no colour unless it is being evacuated, so "needs the barrier"
    // is exactly "tagged", and every consumer spells that as one of the two predicates below.
    // Those two share a blind spot: an untagged value satisfies neither, so an if/else-if chain
    // over them lets it through unexamined. That is correct while good == 0 and wrong the moment
    // a good colour is non-zero, which is what phase C does.
    //
    // IsLoadBad is declared on Collector (Collector.h) so the six phase barriers, which hold a
    // Collector&, can spell it. Phase C changes that one body -- as in ZGC's
    // ZPointer::is_load_bad, zAddress.inline.hpp:626-628 -- instead of ~90 call sites.

    // note this api is not atomic, caller should take care of this.
    // Stale remap colour (ZGC: the value itself says it may be stale). No pointer tagID.
    bool IsOldPointer(RefField<>& ref) const override { return IsLoadBad(ref); }

    // note this api is not atomic, caller should take care of this.
    // Current colour: has the remap bit being handed out now. Plain (no colour) is neither.
    bool IsCurrentPointer(RefField<>& ref) const override { return ZPointer::is_load_good(ref.GetFieldValue()); }

    // OpenJDK ZPointer::is_young_load_good/is_old_load_good
    // (zAddress.inline.hpp:648-655): the conceptual generation epoch is represented by the two
    // accepted bits in that generation's mask.


    // OpenJDK ZBarrier::remap_generation (zBarrier.inline.hpp:110-137): one generation-good
    // bit identifies the other generation; a double-bad colour consults the forwarding side table.
    ZGenerationId remap_generation(RefField<>& ref) const override
    {
        CHECK_DETAIL(!ZPointer::is_load_good(ref.GetFieldValue()), "load-good reference does not need remap");
        if (ZPointer::is_old_load_good(ref.GetFieldValue())) return ZGenerationId::young;
        if (ZPointer::is_young_load_good(ref.GetFieldValue())) return ZGenerationId::old;
        // zBarrier.inline.hpp:124-136: the remembered bits disambiguate old
        // heap fields; otherwise test the young generation's forwarding map.
        if ((raw(ref.GetFieldValue()) & ZPointerRememberedMask) == ZPointerRememberedMask) {
            return ZGenerationId::old;
        }
        const MAddress address = raw(ref.GetTargetObject());
        if (address == 0) {
            return ZGenerationId::old;
        }
        if (Heap::GetHeap().GetCollector().GetZGeneration(Generation::Young).forwarding_table().get(address) != nullptr) {
            return ZGenerationId::young;
        }
        return ZGenerationId::old;
    }

    // OpenJDK ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp:131-140): an address
    // outside the selected generation's forwarding table is already safe; a matching entry routes
    // to the current object. The generation check prevents an address-reuse alias from selecting a
    // route installed by the other generation.
    //
    // ZRelocate::relocate_object (zRelocate.cpp:382-410) has three exits only:
    //   ① find() hit → to    ② retain+copy → to    ③ wait, then forward_object
    // forward_object (zRelocate.cpp:412-416) asserts find()!=null. There is no
    // "return from". non-heap / no-ghost are "not in this forwarding table"
    // (zGeneration.inline.hpp:131-140), not a fourth relocate exit.
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const override
    {
        return relocate_or_remap_object(
            obj, generation,
            ForwardingProvenance{ ForwardingHolderKind::StackSlot, this, &obj });
    }

    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation,
                                         const ForwardingProvenance& provenance) const override
    {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) return obj;
        const MAddress from = reinterpret_cast<MAddress>(obj);
        const Generation ownerGeneration = generation == ZGenerationId::young
            ? Generation::Young : Generation::Old;
        ZForwarding* forwarding = Heap::GetHeap().GetCollector().GetZGeneration(ownerGeneration).forwarding_table().get(from);
        if (forwarding == nullptr) return obj;

        // zRelocate.cpp:383-415: lookup, retain/copy/release, then wait/find.
        // Every leg carries the same set-owned forwarding, including after
        // the source page has been released and its descriptor reused.
        if (const MAddress to = forwarding->find(from)) {
            return reinterpret_cast<BaseObject*>(to);
        }
        ZPage::RetainScope lease{forwarding};
        if (lease.ok()) {
            if (BaseObject* to = TryMutatorRelocate(obj, lease)) return to;
        }
        lease.Release();
        BaseObject* to = WaitForPageForwarding(obj, lease.HoldForwarding());
        if (to == nullptr) {
            FailClosedLoad("ZRelocate::forward_object requires a forwarding entry", obj, 0, provenance);
        }
        return to;
    }

    void AddRawPointerObject(BaseObject* obj) override
    {
        (void)PinRawPointerObject(obj);
        // ⚠ Callers of this void form (Sync futures/mutexes) keep using the pointer they
        // passed in. If that pointer was a movable from-copy resolved above, their later
        // RemoveRawPointerObject would Dec the from region — same pairing hazard as
        // oracleblack face c. Sync objects are pinned at creation (never from) today;
        // adopting the resolved pointer there is a tracked follow-up, not done here.
    }

    BaseObject* PinRawPointerObject(BaseObject* obj) override
    {
        // oracle R4 / RegionManager.h:507: do not pin a movable from-copy
        // during PREFORWARD/FORWARD (CHECK would fire). Resolve to `to` first;
        // a true VisitLive hole is already Exempt-kept, TryDeleteRegion(FROM)
        // fails and the else arm is taken. CHECK is not relaxed.
        //
        // oracleblack round 10, face c: the resolved pointer MUST flow back to the
        // caller. Inc lands on region(to); MCC_ReleaseRawData Decs the region of the
        // payload pointer the caller kept. Pinning to while handing out from both
        // underflowed the from region's count and gave C a payload the young cycle
        // was about to relocate.
        if (obj != nullptr && Heap::IsHeapAddress(obj)) {
            const MAddress addr = reinterpret_cast<MAddress>(obj);
            if (GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
                GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr) {
                const ForwardingProvenance provenance{
                    ForwardingHolderKind::HeapRef, this, &obj
                };
                obj = ValidateCurrentValue(obj, provenance);
            }
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.AddRawPointerObject(obj);
        return obj;
    }

    void RemoveRawPointerObject(BaseObject* obj) override
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.RemoveRawPointerObject(obj);
    }

    void ResolveCycleRef() override;

    // BaseObject* ForwardFixRefField(RefField<>& field) const;
    // lonefrom: "is this object being relocated in this cycle" must not be asked as
    // "is its region still typed FROM_REGION".  ForwardFromRegions takes each region off the
    // from-list with TakeHeadRegion() (RegionManager.cpp:1638), so a
    // region is retyped the moment relocation of it starts.  IsFromRegion() tests FROM_REGION
    // alone -- IsLoneFromRegion() is a separate predicate -- so for the whole window in which a
    // region is actually being evacuated, its objects answer "not from".
    //
    // What that produces: an object in a LONE_FROM region that has not been copied yet answers
    // false to all three staleness authorities (IsFromObject, IsGhostFromObject, IsForwarded --
    // the last only becomes true after the copy), so GetAndTryTagRefField paints it the *current*
    // remap colour.  The slot is then load-good; the object is copied a moment later; and the read
    // barrier's fast path hands the from-version straight to the mutator, which reads its header
    // as one 64-bit word and gets ObjectState::FORWARDED in bits 48-49.
    //
    // afterFlip=1 (slot colour == current good colour, i.e. painted after the relocate-start flip),
    // slotGood=1, hasTo=1 (a to-version exists), unmov=0 -- and unmov=0 is itself explained here,
    // since IsUnmovableFromObject covers UNMOVABLE_FROM/RAW_POINTER_PINNED and not LONE_FROM.
    //
    // OpenJDK never asks a page-type enum this question.  Relocation-set membership is decided once
    // when the set is installed (zGeneration.cpp:254) and answered per address through
    // ZForwardingTable, so it cannot change under a concurrent reader the way a region type does.
    static constexpr bool kLoneFromIsFrom = true;
    mutable std::atomic<uint64_t> loneFromHits{ 0 };

    // PORT_ZFORWARDING step 2: membership answered by address, the way ZGC answers it
    // (ZGeneration::relocate_or_remap_object -> _forwarding_table.get(addr)).  Step 1 established
    // the two answers are equivalent: 1.68e8 comparisons across 10 runs, tableOnly=0 legacyOnly=0.
    //
    // The old path stays as the control arm.  What changes is *which* answer is authoritative: a
    // region type is rewritten as relocation progresses, and a predicate reading it can be right
    // one instant and wrong the next -- that shape produced several of this session's dead ends.
    // An address either is in the set or is not.
    static constexpr bool kMembershipFromTable = true;

    bool IsFromObject(BaseObject* obj) const override
    {
        if (kMembershipFromTable) {
            if (!Heap::IsHeapAddress(obj)) {
                return false;
            }
            const MAddress addr = reinterpret_cast<MAddress>(obj);
            return GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
                   GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr;
        }
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            auto regionInfo = Heap::page(reinterpret_cast<uintptr_t>(obj));
            if (kLoneFromIsFrom && regionInfo->IsLoneFromRegion()) {
                // Arm self-check: a null result from this change is only readable if the branch is
                // known to fire.  Powers of two, so a hot predicate cannot flood the log.
                const uint64_t n = loneFromHits.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    LOG(RTLOG_ERROR, "[LONEFROM] trigger n=%lu", n);
                }
                return true;
            }
            return regionInfo->IsFromRegion();
        }

        return false;
    }

    bool IsGhostFromObject(BaseObject* obj) const override
    {
        return IsFromObject(obj);
    }

    bool IsUnmovableFromObject(BaseObject* obj) const override;

    // Refuses a non-heap address the way FindToVersion does below, and for the same reason:
    // GetGhostFromRegionAt -> GetUnitIdxAt has no heap range
    //
    // Old-tagged fields are exactly where non-heap payloads appear -- a TypeInfo*, a binary
    // constant, immortal metadata: after Flip their colour is IsOldPointer while the payload is
    // still the live non-heap pointer. Every
    // caller's *load-good* arm gated on Heap::IsHeapAddress before looking the route up; none of
    // the *old-tag* arms did.  Guarding here rather than at each arm is a shared guard:
    //
    //   ResolveMinorReference(RefField&) ra1=ResolveMinorReference    (where it moved to)
    //   ResolveMinorReference(RootSlot&) same shape
    //
    // Reproduced 10/10 with cjcj::cjc --package packages/basic/src --output-type=staticlib on a
    // coloured host runtime:
    //   F GetUnitIdxAt OOB addr=0x6282f2cd8c40 heap=[0x719247600000, 0x719257600000)
    // 0x6282f2... is the compiler's own image, the same range as start_ip in that run's stack-map
    // lines.  Gating only the first site moved the abort to the second, which is what showed the
    // population was the old-tag paths rather than one call site.
    FindToVersionResult FindToVersion(BaseObject* obj, Generation generation) const override
    {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return FindToVersionResult::NotManaged();
        }
        const MAddress to = forwarding_find(generation, reinterpret_cast<MAddress>(obj));
        return to != 0 ? FindToVersionResult::Found(reinterpret_cast<BaseObject*>(to))
                       : FindToVersionResult::NotForwarded();
    }

protected:
    void CheckStoreGoodTarget(const char* consumer, BaseObject* target,
                              const ForwardingProvenance& provenance) const;
    // zRelocate.cpp:354-379 relocate_object_inner: find hit → return; else
    // alloc (or reuse a prepared dest) → copy → insert; CAS loser uses winner.
    BaseObject* RelocateObjectInner(BaseObject* obj, ZPage* copyPage);
    void UpdateRemsetForFields(BaseObject* from, BaseObject* to);

    // portmutreloc: ZRelocate::relocate_object's middle leg (zRelocate.cpp:391-406) --
    // retain the from-region, relocate the object on this thread, release. Returns the
    // to-version, or nullptr when the owning copier must supply the receipt.
    BaseObject* TryMutatorRelocate(BaseObject* from, ZPage::RetainScope& lease) const;

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const override;

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const override;
    bool TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& newRef,
                                         const ForwardingProvenance& provenance) const override;
    bool TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const override;

    // ── store value side: typed, mirroring ZGC ────────────────────────────────────────────
    //
    // ZGC colours only a zaddress -- ZAddress::color(zaddress addr, uintptr_t color) -- and the
    // sole producer of a zaddress is ZPointer::uncolor, which asserts
    //     is_load_good(ptr) || is_null_any(ptr)   "Should be load good when handed out"
    // (zAddress.inline.hpp:609-614).  A from-version address therefore cannot reach a store: the
    // type system rejects it, with no runtime check on the hot path.
    //
    // Our slots were typed by ad5c28b3 (HeapSlot/RootSlot/DerivedSlot) but the *value* side was
    // not: everything took a bare BaseObject*, which means both "the live to-version" and "some
    // managed pointer I happen to hold".  That is the same conflation MAddress = Uptr had, one
    // level up, and it let the colouring code paint a from-version with the current remap colour.
    // Measured, N=5 on cjcj::cjc --package packages/basic/src: every run installs load-good slots
    // naming FORWARDED targets (>=1, >=16, >=16, >=16, >=32 by the powers-of-two sampler), 21 of
    //
    // So the colouring code is now reachable only through this typed pair.  To paint the current
    // colour a caller must hold a zaddress, and the only producer is ClassifyStoreValue below,
    // which carries the proof.  "Paint a from-version store-good" no longer compiles.

    // Sole colouring site for a value already proven to be the live to-version.
    // Store-good colour: mark-good | current Remembered (OpenJDK zAddress.cpp:83).
    // Produce the load-bad member of the full-colour family without changing
    // the address.  This is deliberately separate from GetAndTryTagRefField:
    // unclassified store values still have to pass ResolveStoreValue first.
    RefField<> ColourStaleLoadBad(zaddress_unsafe stale) const
    {
        const Uptr notCurrent = ZPointerRemappedMask ^ ZPointerRemapped;
        const Uptr staleOneHot = notCurrent & -notCurrent;
        const Uptr storeColour = ZPointer::remap_bits(staleOneHot) | ZPointerMarkedYoung | ZPointerMarkedOld | ZPointerRemembered;
        return RefField<>(from_region_addr(raw(stale)), storeColour);
    }

    // The ONE adapter from a raw managed pointer to the typed pair.  This is where the proof that
    // ZPointer::uncolor asserts gets actually established, instead of being assumed by
    // ColourTypes.h's from_object ("凭什么: a live BaseObject* in hand"), which checks nothing.
    //
    // ⭐ The authority is the object's own state word, not the region's type.  IsFromObject /
    // IsGhostFromObject ask the *region* whether it is from-space, and a region type is a moving
    // property: once the cycle retires the region becomes TO/RECENT_FULL while the from-version
    // still sits in it with FORWARDED in its header (StateWord.h:22-30 FORWARDED = 3;
    // StateWord.h:174-178 puts stateCode at bits 48-49, which is why a stale value read with the
    // compiler's bare `mov (%rbx),%rdi` faults non-canonically as #GP rather than as a page
    // fault).  Same defect shape as the remset condition fixed in abe3c4d8 -- a predicate testing
    // a property that moves instead of the object's own authoritative state.  ZGC never asks the
    // page either: is_load_good compares the pointer's colour to the global remap colour, and
    // forwarding identity is a per-address ZForwarding lookup.
    // ZGC resolves a field word exactly once.  ZBarrier::make_load_good is the resolve
    // (zBarrier.inline.hpp:294-343); the colouring step that follows it is a pure recolour --
    // ZPointer::uncolor / ZAddress::store_good rebuild the word from the address they were
    // handed and never consult the forwarding table (zAddress.inline.hpp:609-624,806-811).
    //
    // GetAndTryTagRefField resolves again, and under in-place compaction that second resolve is
    // not idempotent: from- and to-addresses share one page span, so a destination this page
    // already produced is itself a from-index of the same table and the lookup shifts it a
    // second time.  Measured on NW256, one 512-element reference array, 3/3: 383 elements
    // rewritten from the correct current address to that address minus the page's own
    // compaction delta, every one of them by this step, with the load-good funnel taking no
    // remap exit at all.
    //
    // The three checks are kept, and they now carry the invariant instead of a resolve: the
    // address handed to the colour producer is already resolved, and an unresolved one stops
    // here rather than being laundered into a store-good word.
    RefField<> ColourResolvedRefField(BaseObject* target,
                                      const ForwardingProvenance& provenance = {}) const
    {
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        if (!Heap::IsHeapAddress(target)) {
            return RefField<>(target);
        }
        CheckStoreGoodTarget("ColourResolvedRefField", target, provenance);
        if (kColourWhoProbe) {
            NoteStoreGoodOnBadTarget(target);
        }
        return RefField<>(ZAddress::store_good(from_object(target)));
    }

    RefField<> GetAndTryTagRefField(BaseObject* target) const override
    {
        const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, target, &target };
        return GetAndTryTagRefFieldWithProvenance(target, provenance);
    }

    RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* target,
                                                  const ForwardingProvenance& provenance) const override
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad).
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        // TypeInfo* / binary constants / immortal metadata are not relocated,
        // but a non-null HeapSlot word is still coloured.  The load-good mask
        // fast path peels it without routing through the collector.
        if (!Heap::IsHeapAddress(target)) {
            return RefField<>(ZAddress::store_good(from_object(target)));
        }
        // ZPointer::uncolor is the sole producer accepted by ZAddress::store_good
        // (zAddress.inline.hpp:609-624,806-811). ResolveStoreValue is our
        // make-load-good producer: a relocation-set address is looked up or copied
        // by this thread; an unresolved address never reaches colouring.
        target = ValidateCurrentValue(target, provenance);
        CHECK_DETAIL(target != nullptr && Heap::IsHeapAddress(target),
                     "store-good requires a resolved heap address");
        CheckStoreGoodTarget("GetAndTryTagRefField", target, provenance);
        // colourwho: installed-slot checking sits after Barrier::WriteReference, so it only sees the
        // mutator store path.  That path now measures ~0 while the read barrier still hands out
        // load-good slots naming from-versions, which means the writer is on the *collector* side --
        // preforward/ref_fix/self-heal all colour through here too.  This is the single funnel for
        // every coloured value in the runtime, so the count belongs here.
        //
        // Fires when we are about to paint the current (load-good) colour on a target whose own
        // header already says FORWARDED, or whose header is zeroed.  Both are the crash families.
        if (kColourWhoProbe) {
            NoteStoreGoodOnBadTarget(target);
        }
        return RefField<>(ZAddress::store_good(from_object(target)));
    }

    // holdermark: probe-only view of the old-generation mark bit.
    bool IsMarkedObjectForProbe(BaseObject* obj) const override
    {
        return IsMarkedObject<Generation::Old>(obj);
    }

    // flipwitness: the probe needs the colour currently handed out, without reaching into members.
    Uptr CurrentRemapColourForProbe() const override { return ZPointerRemapped; }

    // colourwho: compile-time gated -- this is the funnel every coloured write goes through.
    static constexpr bool kColourWhoProbe = true;

    void NoteStoreGoodOnBadTarget(BaseObject* target) const
    {
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
        const unsigned stateCode = static_cast<unsigned>((hdr >> 48) & 0x3u);
        const uint64_t typeInfo = hdr & 0xffffffffffffull;
        const uint64_t seen = colourWhoTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen == 1) {
            // Positive control: a zero below must not be readable as a dead probe.
            LOG(RTLOG_ERROR, "[COLOURWHO] armed first sc=%u", stateCode);
        }
        if (stateCode == 0 && typeInfo != 0) {
            return;
        }
        const uint64_t bad = colourWhoBad.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((bad & (bad - 1)) != 0) {
            return;
        }
        LOG(RTLOG_ERROR, "[COLOURWHO] bad=%lu of %lu target=%p sc=%u typeInfo=0x%lx isFrom=%d isGhost=%d phase=%d",
            bad, seen, static_cast<void*>(target), stateCode, typeInfo, IsFromObject(target) ? 1 : 0,
            IsGhostFromObject(target) ? 1 : 0, static_cast<int>(Heap::GetHeap().GetGCPhase(GCCycleGeneration::OLD)));
    }
    mutable std::atomic<uint64_t> colourWhoTotal{ 0 };
    mutable std::atomic<uint64_t> colourWhoBad{ 0 };

    // A store value is stale if *either* authority says so: the region it sits in is from-space,
    // or the object's own state word says it has been forwarded.  Region type moves; the state
    // word does not, so asking only the region is what let load-good slots name FORWARDED targets.
    // ZBarrier::is_good_or_null_fast_path keeps an already-remapped value out
    // of the from-side slow path (zBarrier.inline.hpp:294-343).  The equivalent
    // invariant here is content plus current relocation-set membership: a
    // usable managed object outside every current from range is already the
    // address to hand out.  In particular, a retired carrier may still cover
    // that numerical address; consulting FindToVersion would reinterpret the
    // current to-version as a historical from-version.  Do not turn a lookup
    // miss into success here: current from-range members remain on the normal
    // receipt/relocate path and therefore retain fail-closed handling.
    bool IsAlreadyToStoreValue(BaseObject* target, Generation generation) const
    {
        return target != nullptr && Heap::IsHeapAddress(target) &&
            Collector::JudgeHandOutTarget(target) == HandVerdict::Usable &&
            generation_forwarding_table(generation).get(reinterpret_cast<MAddress>(target)) == nullptr;
    }



    // Typed, unconditional boundary: HeapSlot write-back is coloured while
    // stack/register/static RootSlot write-back stays plain, matching ZGC's
    // ZUncoloredRoot.  There is no runtime switch between the two contracts.
    RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const
    {
        return GetAndTryTagRefField(target);
    }

    void CollectLargeGarbage()
    {
        MRT_PHASE_TIMER(ZStatPhases::PCollectLargeGarbage);
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.largeSpaceSize = space.LargeObjectBytes();
        stats.largeGarbageSize = space.CollectLargeGarbage();
        stats.collectedBytes += stats.largeGarbageSize;
    }

    void CollectPinnedGarbage()
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        GCStats& stats = GetGCStats();
        stats.pinnedSpaceSize = space.PinnedSpaceSize();
        stats.pinnedGarbageSize = space.CollectPinnedGarbage();
        stats.collectedBytes += stats.pinnedGarbageSize;
    }

    void CollectSmallSpace();

    void DoGarbageCollection(GCCycleGeneration generation) override;
    void ProcessFinalizers() override;
    void EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet, Generation generation) const override;

private:
    using MinorObjectSet = std::unordered_set<BaseObject*>;
    using MinorRegionSet = std::unordered_set<ZPage*>;
    using MinorSlotSet = std::unordered_set<MAddress>;
    using MinorInteriorBaseMap = std::unordered_map<MAddress, BaseObject*>;

    bool MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache = nullptr) const;
    bool MarkYoungObject(BaseObject* obj, MarkLiveCache* liveCache = nullptr) const
    {
        return MarkObjectImpl(obj, true, liveCache);
    }

    // Large reference-array chunking, ported from ZGC's ZMark (zMark.cpp:185-263).
    // `holder` is the owning array, carried only for diagnostics; it is nullptr
    // when the elements arrive as a chunk popped off the work stack, because
    // ZGC's partial-array entry does not carry the holder either.
    void PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack, bool finalizable = false) const;
    void FollowArrayElementsSmall(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack, bool finalizable) const;
    void FollowArrayElementsLarge(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack, bool finalizable) const;
    void FollowArrayElements(BaseObject* holder, RefField<>* addr, size_t length,
                             WorkStack& workStack, bool finalizable) const;

    bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                  bool allowNull = false) const;
    BaseObject* ResolveMinorReference(RefField<>& field,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    BaseObject* ResolveMinorReference(RootSlot& root,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    void VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
                             uint64_t stackScanEpoch = 0);
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                         const std::function<void(BaseObject*)>& invisibleVisitor,
                         uint64_t stackScanEpoch = 0);
    // origin tags root source for invalid-minor-root diagnosis (gcbadroot).
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown") const;
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin, bool finalizable) const;
    // setbitmap O1③: claim young via MarkObject (region mark bitmap) + collect vector;
    // FYS=0 skips reachableSlots inserts (slots never looked up). Object claims use the bitmap.
    void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                           MinorSlotSet& weakSlots,
                           const MinorSlotSet* reachableSlotDomain = nullptr);
    void TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                  std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                  MinorSlotSet& weakSlots,
                                  const MinorSlotSet* reachableSlotDomain = nullptr);
    // Follow this generation's published mark work through TraceYoungClosure.
    bool FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
                             std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                             MinorSlotSet& weakSlots,
                             YoungConcWindowStats* windowStats = nullptr);
    // ZMark::try_end sibling: called with mutators stopped; performs exactly one
    // local-buffer flush and reports whether concurrent-mark-continue is needed.
    bool TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats = nullptr);
    friend class ZMarkTask;
    bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    void FixMinorRootSlots(const ScopedStopTheWorld* stw = nullptr);
    void FixMinorObjectSlots(BaseObject* object, const ScopedStopTheWorld* stw = nullptr);
    // stw: live handle lets relocate follow ZGC Phase 7/8 (zGeneration.cpp:573-580):
    // pause = flip + phase + root fix; concurrent = ForwardFromSpace; re-STW = heap
    // slot catch-up + evac_finish. nullptr keeps the whole evacuate under the caller STW.
    void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec, const MinorSlotSet& rememberedSlots, bool refFixSlotsCoveredByReachable,
                              const MinorInteriorBaseMap& interiorBases,
                              std::unique_ptr<ScopedStopTheWorld>* stw = nullptr);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void DoYoungGarbageCollection();
    void DoYoungGarbageCollectionBody();
    // After nested young, remaining young survivors hold young→old edges the
    // young closure skipped. ZGC overlapping mark paints old targets from those
    // stores (zBarrier.inline.hpp:742-749). Seed them into the old TRACE stack.
    void FlushAllocationRegions();
    template<bool forward>
    bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef,
                               const ForwardingProvenance& provenance) const;
    void TraceHeap();
    void PostTrace();
    // OpenJDK ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1523):
    // before old relocate-start flip, remap young roots + remset so none carry
    // two remap-bit errors.
    void RemapYoungRoots();
    bool Preforward();
    void StartRelocationTasks(GCCycleGeneration generation);
    BaseObject* WaitForPageForwarding(BaseObject* obj, ZForwarding* owner) const;
    void PreforwardDiscoveredExternObjects(Generation generation);
    void PreforwardAllResurrectExportFromObjects(Generation generation);
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);
#if defined(MRT_GC_UNIT_TESTS)
    CrossRefHandler cycleRefHandlerForTest = nullptr;
#endif

    ForwardTable fwdTable;
    // gc index 0 or 1 is used to distinguish previous gc and current gc.
    uint64_t minorTotalRuns = 0;
    MinorRegionSet minorCandidateRegions;
};
} // namespace MapleRuntime
#endif // ~MRT_WCOLLECTOR_H
