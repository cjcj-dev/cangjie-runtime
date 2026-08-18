// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Common/ColourMask.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "Allocator/RegionSpace.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Collector/CopyCollector.h"
#include "Heap/Verify/DiffPathExplainer.h"
#include "Heap/Verify/ToverFailDiag.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
class ScopedStopTheWorld;

// paramzero: crash-time dump of Mode-A frame slot + heap CAS-null counters.
// Declared here so SignalManager can call without including WCollector.cpp guts.
// Gate = MRT_GCV2_NULLSLOT (default off).
void EmitParamzeroCrashProbe(uintptr_t rbp, uintptr_t rbx, uintptr_t rip);

// portyoungconc: work accounting for the concurrent young mark window.
// ZGC anchor: ZGenerationYoung::concurrent_mark() = mark_roots() + mark_follow()
// (zGeneration.cpp:665-669) — everything a young collector does between
// pause_mark_start and pause_mark_end runs with mutators alive.
// Every field below counts GC work performed *while the world is running*, so the
// closed arm (MRT_GCV2_YOUNG_CONC_MARK unset) reports all-zero by construction:
// there is no window there. A non-zero windowNs with markedInWindow()==0 and
// satbObjects==0 means the window exists but carries no marking work — report that,
// do not read a duration as evidence of concurrency.
struct YoungConcWindowStats {
    uint64_t windowNs = 0;    // world-released → STW2 requested
    size_t satbObjects = 0;   // objects popped out of SATB inside the window
    size_t satbIters = 0;     // SATB termination loop iterations inside the window
    size_t closureCalls = 0;  // TraceYoungClosure invocations inside the window
    size_t markedAtEntry = 0; // reachableVec.size() at world-release
    size_t markedAtExit = 0;  // reachableVec.size() at STW2 request
    size_t remsetSlots = 0;   // remset slots consumed by the in-window rescan
    size_t reenters = 0;      // ZGC pause_mark_end() == false → concurrent_mark_continue()
    size_t MarkedInWindow() const { return markedAtExit >= markedAtEntry ? markedAtExit - markedAtEntry : 0; }
};

class ForwardTable {
public:
    explicit ForwardTable(RegionSpace& space) : theSpace(space) {}

    RoutePlan PlanRoute(BaseObject* old, CopierRouteToken token)
    {
        return theSpace.GetRegionManager().PlanRoute(old, token);
    }

    // if region is compacted, return false.
    bool RouteRegion(RegionInfo* region) { return theSpace.GetRegionManager().RouteRegion(region); }

    template<Generation G>
    void PrepareForwardTable()
    {
        DLOG(FORWARD, "reset fwd table");
        theSpace.PrepareFromSpace<G>();

        ForwardDataManager::GetForwardDataManager().ClearPreviousForwardData();
    }

    RegionSpace& theSpace;
};

using CrossRefHandler = void(*)(BaseObject*, BaseObject*);

class WCollector : public CopyCollector {
public:
    explicit WCollector(Allocator& allocator, CollectorResources& resources)
        : CopyCollector(allocator, resources), fwdTable(reinterpret_cast<RegionSpace&>(allocator))
    {
        collectorType = CollectorType::SMOOTH_COLLECTOR;
    }

    ~WCollector() override = default;

    void Init() override { ForwardDataManager::GetForwardDataManager().InitializeForwardData(); }

    void MarkNewObject(BaseObject* obj) override;

    bool ShouldIgnoreRequest(GCRequest& request) override;
    bool MarkObject(BaseObject* obj) const override;
    bool ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* regionInfo) override;

    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void TraceRefField(BaseObject* obj, RefField<>& ref, WorkStack& workStack) const;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack) override;
    void FollowPartialArray(BaseObject* entry, WorkStack& workStack) override;
    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field) override;
    BaseObject* ForwardObject(BaseObject* fromVersion) override;
    void PostResolveCycleTask();
    void PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        cycleRefWorkStack.insert(discoveredExternObjects.begin(), discoveredExternObjects.end());
        discoveredExternObjects.clear();
    }
    void MergeResurrectExportObjects()
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }
    void FlipTagID() { currentTagID = static_cast<uint16_t>((currentTagID + 1) % TAG_ID_COUNT); }
    uint16_t GetCurrentTagID() override { return currentTagID; }
    uint16_t GetPreviousTagID() const
    {
        return static_cast<uint16_t>((currentTagID + TAG_ID_COUNT - 1) % TAG_ID_COUNT);
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

    // The colour currently handed out. Flipping a phase swaps it and updates the mask the
    // compiler tests, which is what replaces walking the heap to strip stale colours.
    Uptr ZPointerRemappedYoungMask = ZPointerRemapped10 | ZPointerRemapped00;
    Uptr ZPointerRemappedOldMask = ZPointerRemapped01 | ZPointerRemapped00;
    Uptr currentRemapColour = ZPointerRemapped00;
    Uptr currentMarkedYoung = MARKED_YOUNG_0;
    Uptr currentMarkedOld = MARKED_OLD_0;
    // OpenJDK ZPointerRemembered (zAddress.cpp:125); flips with young mark start (:133-134).
    Uptr currentRemembered = REMEMBERED_0;
    size_t youngMarkFlipCount = 0;
    size_t oldMarkFlipCount = 0;

    // The five epoch words this collector currently hands out, as the POD the shared formula
    // takes. Order matches EpochColours (ColourMask.h) and the member declarations above.
    EpochColours current_epoch_colours() const
    {
        return EpochColours{ static_cast<uintptr_t>(ZPointerRemappedYoungMask),
                             static_cast<uintptr_t>(ZPointerRemappedOldMask),
                             static_cast<uintptr_t>(currentMarkedYoung),
                             static_cast<uintptr_t>(currentMarkedOld),
                             static_cast<uintptr_t>(currentRemembered) };
    }

    // Mirrors ZGlobalsPointers::set_good_masks (OpenJDK zAddress.cpp:78-94):
    //   :81 LoadGood  = remap_bits(Remapped)
    //   :82 MarkGood  = LoadGood | MarkedYoung | MarkedOld
    //   :83 StoreGood = MarkGood | Remembered
    // Bad masks are Good ^ Metadata (+ tagged for our ABI). Finalizable not introduced
    // (ColourMask.h kFinalizableWired).
    //
    // c4unify: the arithmetic moved to ColourMask.h ComputeBadMasks so that this function and
    // the three static initialisers in BaseObject.cpp stop being two independent copies of the
    // same formula. Publication order and the set of published words are unchanged; the four
    // flip_* functions below and their six call sites are untouched on purpose -- a table
    // cannot see "a flip forgot to call set_good_masks", so that stays a separate change.
    // flipseq: a monotonic count of colour publications.  The remap space is four values and a flip
    // is an xor, so a colour that was good at publication N is good again at N+2 for that
    // generation's mask -- "the slot recycled into good" and "the slot was painted after the target
    // moved" produce identical colours and can only be told apart by *when*.  ZGC has the same
    // four-value space (zAddress.hpp:59-130) so this is not a divergence by itself; it is the axis
    // on which the two stories separate.
    static std::atomic<uint64_t>& FlipSeq()
    {
        static std::atomic<uint64_t> seq{ 0 };
        return seq;
    }

    void set_good_masks()
    {
        FlipSeq().fetch_add(1, std::memory_order_relaxed);
        const EpochColours e = current_epoch_colours();
        const BadMasks m = ComputeBadMasks(e);
        currentRemapColour = m.remapColour;
        ::g_cjLoadBadMask = m.loadBad;
        ::g_cjMarkBadMask = m.markBad;
        ::g_cjStoreBadMask = m.storeBad;
    }

    // OpenJDK ZGlobalsPointers::flip_young_relocate_start/flip_old_relocate_start
    // (zAddress.cpp:138-151): each generation independently alternates the two accepted pairs.
    void flip_young_relocate_start()
    {
        ZPointerRemappedYoungMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
    }

    void flip_old_relocate_start()
    {
        ZPointerRemappedOldMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
    }

    // OpenJDK zAddress.cpp:132-136: young mark-start flips MarkedYoung and Remembered together.
    void flip_young_mark_start()
    {
        currentMarkedYoung ^= MARKED_YOUNG_MASK;
        currentRemembered ^= REMEMBERED_MASK;
        set_good_masks();
        if (++youngMarkFlipCount == 1) {
            LOG(RTLOG_ERROR,
                "[ZCOLOR2][mark-mask-flip] generation=young count=%zu g_cjMarkBadMask=%#lx g_cjStoreBadMask=%#lx",
                youngMarkFlipCount, ::g_cjMarkBadMask, ::g_cjStoreBadMask);
        }
    }

    void flip_old_mark_start()
    {
        currentMarkedOld ^= MARKED_OLD_MASK;
        set_good_masks();
        if (++oldMarkFlipCount == 1) {
            LOG(RTLOG_ERROR, "[ZCOLOR2][mark-mask-flip] generation=old count=%zu g_cjMarkBadMask=%#lx",
                oldMarkFlipCount, ::g_cjMarkBadMask);
        }
    }

    // note this api is not atomic, caller should take care of this.
    // Stale remap colour (ZGC: the value itself says it may be stale). No pointer tagID.
    bool IsOldPointer(RefField<>& ref) const override { return IsLoadBad(ref); }

    // note this api is not atomic, caller should take care of this.
    // Current colour: has the remap bit being handed out now. Plain (no colour) is neither.
    bool IsCurrentPointer(RefField<>& ref) const override { return is_load_good(ref); }

    // OpenJDK ZPointer::is_young_load_good/is_old_load_good
    // (zAddress.inline.hpp:648-655): the conceptual generation epoch is represented by the two
    // accepted bits in that generation's mask.
    bool is_young_load_good(RefField<>& ref) const override
    {
        // 凭什么 raw: 掩码测位型，不解引用。
        return (raw(ref.GetFieldValue()) & ZPointerRemappedYoungMask) != 0;
    }

    bool is_old_load_good(RefField<>& ref) const override
    {
        return (raw(ref.GetFieldValue()) & ZPointerRemappedOldMask) != 0;
    }

    // OpenJDK ZBarrier::remap_generation (zBarrier.inline.hpp:110-137): one generation-good
    // bit identifies the other generation; a double-bad colour consults the forwarding side table.
    ZGenerationId remap_generation(RefField<>& ref) const override
    {
        CHECK_DETAIL(!is_load_good(ref), "load-good reference does not need remap");
        bool youngLoadGood = is_young_load_good(ref);
        bool oldLoadGood = is_old_load_good(ref);
        if (oldLoadGood && !youngLoadGood) {
            return ZGenerationId::young;
        }
        if (youngLoadGood && !oldLoadGood) {
            return ZGenerationId::old;
        }

        BaseObject* target = to_object(ref.GetTargetObject());
        if (!Heap::IsHeapAddress(target)) {
            return ZGenerationId::old;
        }
        RegionInfo* forwarding = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
        if (forwarding != nullptr && forwarding->generation_id() == ZGenerationId::young) {
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
    // permhit: WaitRoutedTipReady is reachable from here and nowhere else (Collector.h:169
    // make_load_good is its only caller), so "enter=0" alone cannot say whether the wait was
    // never needed or whether the read barrier never reached this funnel at all. Count each
    // arm under the same MRT_GCV2_WAITFWD gate; every counter is behind the gate, so the
    // product path is unchanged when it is off.
    static bool RemapFunnelOn()
    {
        static const int on = []() {
            const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_WAITFWD */;
            return (v != nullptr && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }();
        return on != 0;
    }

    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const override
    {
        static std::atomic<uint64_t> callCount{ 0 };
        static std::atomic<uint64_t> nonHeapCount{ 0 };
        static std::atomic<uint64_t> noGhostCount{ 0 };
        static std::atomic<uint64_t> routeNullCount{ 0 };
        static std::atomic<uint64_t> receiptCount{ 0 };
        static std::atomic<uint64_t> waitCount{ 0 };
        static std::atomic<uint64_t> giveFromCount{ 0 };
        const bool funnel = RemapFunnelOn();
        // toverfail reuses the same arm split as MRT_GCV2_WAITFWD (e49a5bcc), under its
        // own gate so product path is unchanged when both are off.
        const bool tv = ToverFailDiag::Enabled();
        if (funnel) {
            static std::atomic<bool> installed{ false };
            bool expected = false;
            if (installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                std::atexit([]() {
                    std::fprintf(stderr,
                                 "[GCV2][remapfunnel] atexit call=%llu nonHeap=%llu noGhost=%llu "
                                 "routeNull=%llu receipt=%llu wait=%llu giveFrom=%llu\n",
                                 static_cast<unsigned long long>(callCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(nonHeapCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(noGhostCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(routeNullCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(receiptCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(waitCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(giveFromCount.load(std::memory_order_relaxed)));
                    std::fflush(stderr);
                });
            }
            callCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (tv) {
            ToverFailDiag::NoteRemapCall();
        }
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::Role funnelRole = MutatorRelocate::Role::MUTATOR;
            if (IsGcThread()) {
                funnelRole = MutatorRelocate::Role::GC;
            } else if (IsRuntimeThread()) {
                funnelRole = MutatorRelocate::Role::OTHER_RT;
            }
            MutatorRelocate::NoteFunnelCall(funnelRole);
        }
        if (!Heap::IsHeapAddress(obj)) {
            if (funnel) {
                nonHeapCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (tv) {
                ToverFailDiag::NoteRemapNonHeap();
            }
            return obj;
        }
        RegionInfo* forwarding = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (forwarding == nullptr || forwarding->generation_id() != generation) {
            if (funnel) {
                noGhostCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (tv) {
                ToverFailDiag::NoteRemapNoGhost(); // 乙
            }
            return obj;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        BaseObject* to = space.GetRegionManager().FindPublishedRoute(obj, forwarding).dest;
        if (to != nullptr) {
            if (LIKELY(!Heap::IsHeapAddress(to))) {
                if (funnel) {
                    receiptCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (tv) {
                    ToverFailDiag::NoteRemapReceipt();
                }
                return to;
            }
            if ((obj->IsForwarded() || forwarding->IsCompacted()) && to->IsValidObject()) {
                if (funnel) {
                    receiptCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (tv) {
                    ToverFailDiag::NoteRemapReceipt();
                }
                return to;
            }
        } else if (obj->IsForwarded()) {
            BaseObject* published = FindToVersion(obj);
            if (published != nullptr) {
                if (funnel) {
                    receiptCount.fetch_add(1, std::memory_order_relaxed);
                }
                return published;
            }
        }
        // ② retain + copy (zRelocate.cpp:393-400). nullptr = retain refused or copy missed.
        BaseObject* self = TryMutatorRelocate(obj, forwarding);
        if (self != nullptr) {
            return self;
        }
        to = space.GetRegionManager().FindPublishedRoute(obj, forwarding).dest;
        if (to != nullptr && Heap::IsHeapAddress(to) && to->IsValidObject()) {
            if (funnel) {
                receiptCount.fetch_add(1, std::memory_order_relaxed);
            }
            return to;
        }
        // ③ wait then find. ZGC waits only after retain-ok+copy-fail, or inside
        // retain_page when the page is claimed. We wait whenever find is still
        // null. Page-vs-object wait grain is a known difference — not changed here.
        if (funnel) {
            waitCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (tv) {
            ToverFailDiag::NoteRemapWait();
        }
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitEnter();
        }
        BaseObject* waited = WaitRoutedTipReady(obj, to, forwarding);
        if (waited != nullptr && waited != obj) {
            return waited;
        }
        to = space.GetRegionManager().FindPublishedRoute(obj, forwarding).dest;
        if (to != nullptr) {
            if (funnel) {
                receiptCount.fetch_add(1, std::memory_order_relaxed);
            }
            return to;
        }
        // ZRelocate::forward_object: find()==null is an invariant break, not a
        // fourth exit. Never silent return obj (that was the checksum-drift path).
        giveFromCount.fetch_add(1, std::memory_order_relaxed);
        if (funnel) {
            routeNullCount.fetch_add(1, std::memory_order_relaxed);
        }
        LOG(RTLOG_ERROR,
            "[GCV2][remapfunnel] giveFrom from=%p to=%p forwarded=%u route=%u — ZGC asserts here",
            obj, to, static_cast<unsigned>(obj->IsForwarded()),
            static_cast<unsigned>(forwarding->GetRouteState()));
        CHECK_DETAIL(false,
                     "relocate_or_remap_object: unpublished after wait (ZGC forward_object "
                     "assert). from=%p",
                     obj);
        return obj;
    }

    void AddRawPointerObject(BaseObject* obj) override
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.AddRawPointerObject(obj);
    }

    void RemoveRawPointerObject(BaseObject* obj) override
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        space.RemoveRawPointerObject(obj);
    }

    void ResolveCycleRef() override;

    // BaseObject* ForwardFixRefField(RefField<>& field) const;
    BaseObject* ForwardUpdateRawRef(ObjectRef& ref);

    // lonefrom: "is this object being relocated in this cycle" must not be asked as
    // "is its region still typed FROM_REGION".  ForwardFromRegions takes each region off the
    // from-list with TakeHeadRegion(RegionType::LONE_FROM_REGION) (RegionManager.cpp:1638), so a
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
    // Measured before this change, N=5, BarrierPhase::FORWARD hand-outs: 20/20 carry
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
    static constexpr bool kMembershipFromTable = false;

    bool IsFromObject(BaseObject* obj) const override
    {
        if (kMembershipFromTable) {
            if (!Heap::IsHeapAddress(obj)) {
                return false;
            }
            return ForwardingTable::Get(reinterpret_cast<MAddress>(obj)) != nullptr;
        }
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            auto regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
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
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            return RegionInfo::InGhostFromRegion(obj);
        }

        return false;
    }

    bool IsUnmovableFromObject(BaseObject* obj) const override;

    BaseObject* GetForwardPointer(BaseObject* fromObj, RegionInfo* region)
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().FindPublishedRoute(fromObj, region).dest;
    }

    // Refuses a non-heap address the way FindToVersion does below, and for the same reason:
    // PlanRoute -> PlanRouteLookup -> GetGhostFromRegionAt -> GetUnitIdxAt has no heap range
    // check and aborts the process on an address outside the heap.
    //
    // Old-tagged fields are exactly where non-heap payloads appear -- a TypeInfo*, a binary
    // constant, immortal metadata: after Flip their colour is IsOldPointer while the payload is
    // still the live non-heap pointer (FixOldTaggedRefField says so in its own comment).  Every
    // caller's *load-good* arm gated on Heap::IsHeapAddress before looking the route up; none of
    // the *old-tag* arms did.  Guarding here rather than at each arm is one fix instead of four:
    //
    //   FixOldTaggedRefField             ra1=FixOldTaggedRefField     (first abort seen)
    //   ResolveMinorReference(RefField&) ra1=ResolveMinorReference    (where it moved to)
    //   ResolveMinorReference(RootSlot&) same shape
    //   RescanRememberedSet              same shape
    //
    // Reproduced 10/10 with cjcj::cjc --package packages/basic/src --output-type=staticlib on a
    // coloured host runtime:
    //   F GetUnitIdxAt OOB addr=0x6282f2cd8c40 heap=[0x719247600000, 0x719257600000)
    // 0x6282f2... is the compiler's own image, the same range as start_ip in that run's stack-map
    // lines.  Gating only the first site moved the abort to the second, which is what showed the
    // population was the old-tag paths rather than one call site.
    RoutePlan PlanRouteUnderStw(BaseObject* fromObj, const ScopedStopTheWorld& stw) const
    {
        if (fromObj == nullptr || !Heap::IsHeapAddress(fromObj)) {
            return RoutePlan{ nullptr };
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().PlanRoute(fromObj, stw.route_plan_token());
    }

    BaseObject* FindToVersion(BaseObject* obj) const override
    {
        // Mirror IsGhostFromObject: GetGhostFromRegionAt → GetUnitIdxAt has no heap range
        // check, so null / non-heap (incl. colour-only null after flip) aborts as
        // "GetUnitIdxAt OOB addr=0". FixOldTaggedRefField then nulls the slot.
        // nullptr here is dual: non-heap/null gate OR unpublished / no to-version.
        // Soft-resolve paths must not CAS-null on the non-heap reading (RO static).
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return nullptr;
        }
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (fromRegionInfo == nullptr) {
            return nullptr;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().FindPublishedRoute(obj).dest;
    }

protected:
    BaseObject* ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion);
    BaseObject* ForwardObjectExclusive(BaseObject* obj) override;

    // waitfwd: spin until from is FORWARDED (or region COMPACTED); else return from.
    BaseObject* WaitRoutedTipReady(BaseObject* from, BaseObject* to, RegionInfo* forwarding) const;

    // portmutreloc: ZRelocate::relocate_object's middle leg (zRelocate.cpp:391-406) --
    // retain the from-region, relocate the object on this thread, release. Returns the
    // to-version, or nullptr when the caller should fall through to the pre-existing legs
    // (retain refused, wrong phase, or no route). Default off.
    BaseObject* TryMutatorRelocate(BaseObject* from, RegionInfo* forwarding) const;

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const override;

    BaseObject* TryForwardObject(BaseObject* fromVersion);

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const override;
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
    // 22 samples in BarrierPhase::TRACE.
    //
    // So the colouring code is now reachable only through this typed pair.  To paint the current
    // colour a caller must hold a zaddress, and the only producer is ClassifyStoreValue below,
    // which carries the proof.  "Paint a from-version store-good" no longer compiles.

    // Sole colouring site for a value already proven to be the live to-version.
    // Store-good colour: mark-good | current Remembered (OpenJDK zAddress.cpp:83).
    RefField<> ColourStoreGood(zaddress live) const
    {
        const Uptr storeColour = currentRemapColour | currentMarkedYoung | currentMarkedOld | currentRemembered;
        return RefField<>(to_object(live), storeColour);
    }

    // Sole colouring site for a value that is still a from-version.  Paints a *stale* remap
    // one-hot so the slot stays load-bad and the read barrier must resolve it before handing the
    // value to the mutator -- our two-state equivalent of ZGC's "remapped means the address is
    // already the current location" (zAddress.hpp:60-128; mid-evacuation is not a pointer bit).
    RefField<> ColourStaleLoadBad(zaddress_unsafe stale) const
    {
        const Uptr notCurrent = REMAP_COLOUR_MASK ^ currentRemapColour;
        const Uptr staleOneHot = notCurrent & -notCurrent;
        const Uptr storeColour = staleOneHot | currentMarkedYoung | currentMarkedOld | currentRemembered;
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
    RefField<> GetAndTryTagRefField(BaseObject* target) const override
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad).
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        if (IsStaleStoreValue(target)) {
            BaseObject* to = FindToVersion(target);
            if (to != nullptr) {
                target = to;
            }
        }
        if (IsStaleStoreValue(target)) {
            // 凭什么: classification just proved this is *not* a live to-version, which is exactly
            // what zaddress_unsafe means ("uncoloured, NOT safe to dereference").
            return ColourStaleLoadBad(to_zaddress_unsafe(reinterpret_cast<Uptr>(target)));
        }
        // colourwho: installed-slot checking sits after Barrier::WriteReference, so it only sees the
        // mutator store path.  That path now measures ~0 while the read barrier still hands out
        // load-good slots naming from-versions, which means the writer is on the *collector* side --
        // preforward/ref_fix/self-heal all colour through here too.  This is the single funnel for
        // every coloured value in the runtime, so the count belongs here.
        //
        // Fires when we are about to paint the current (load-good) colour on a target whose own
        // header already says FORWARDED, or whose header is zeroed.  Both are the crash families.
        if (kColourWhoProbe) {
            NoteColourStoreGoodOnBadTarget(target);
        }
        return ColourStoreGood(from_object(target));
    }

    // holdermark: probe-only view of the old-generation mark bit.
    bool IsMarkedObjectForProbe(BaseObject* obj) const override
    {
        return IsMarkedObject<Generation::Old>(obj);
    }

    // flipwitness: the probe needs the colour currently handed out, without reaching into members.
    Uptr CurrentRemapColourForProbe() const override { return currentRemapColour; }

    // colourwho: compile-time gated -- this is the funnel every coloured write goes through.
    static constexpr bool kColourWhoProbe = true;

    void NoteColourStoreGoodOnBadTarget(BaseObject* target) const
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
            IsGhostFromObject(target) ? 1 : 0, static_cast<int>(Heap::GetHeap().GetGCPhase()));
    }
    mutable std::atomic<uint64_t> colourWhoTotal{ 0 };
    mutable std::atomic<uint64_t> colourWhoBad{ 0 };

    // A store value is stale if *either* authority says so: the region it sits in is from-space,
    // or the object's own state word says it has been forwarded.  Region type moves; the state
    // word does not, so asking only the region is what let load-good slots name FORWARDED targets.
    // kAskObjectState: compile-time arm switch, same reason as StateWord::kInitStateAtAlloc.
    static constexpr bool kAskObjectState = true;

    // routeask: OpenJDK never asks a region-type enum whether an object is being relocated.
    // ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp:131-140) asks the *address*:
    //     ZForwarding* const forwarding = _forwarding_table.get(addr);
    //     if (forwarding == nullptr) { return safe(addr); }
    //     return _relocate.relocate_object(forwarding, addr);
    // The table is installed once with the relocation set and does not change under a concurrent
    // reader, and when the address *is* in it the load barrier relocates the object right there --
    // so a caller can never come away holding a not-yet-relocated relocation-set object.
    //
    // Ours asks IsFromObject -> regionInfo->IsFromRegion(), a mutable enum that is false both
    // before the region is enrolled and after ForwardFromRegions retypes it to LONE_FROM.  That is
    // the same defect shape as the remset condition (abe3c4d8) and the stillFrom predicate: a
    // question answered by a property that moves, rather than by the authority.
    //
    // This counts, without changing behaviour, how often we are about to paint the current colour
    // on a target whose region already has a route state other than NORMAL -- i.e. exactly the
    // objects ZGC's table would have caught.  Trigger count, not `armed`, is the evidence.
    static constexpr bool kRouteAskProbe = true;
    mutable std::atomic<uint64_t> routeAskHits{ 0 };

    // Full counts, not a sample.  The previous version logged on powers of two and every one of the
    // 105 sampled lines happened to be covered, which was written up as "100% covered" -- a
    // population claim from a sample of the head of the distribution.  An escaping case is by
    // definition rare, so sampling is exactly the wrong instrument for it: count both arms for
    // every call and log only when the uncovered arm moves.
    //
    // routeState numbering matters here and I got it wrong once already:
    // RegionInfo.h:136-143 is NORMAL=0, FORWARDABLE=1, ROUTING=2, ROUTED=3, COMPACTED=4,
    // FORWARDED=5 -- so the enrolment mark is FORWARDABLE, set by PrepareForwardableRegion inside
    // PrepareFromRegionList, which runs from PrepareForwardTable<Old> before the relocate flip.
    mutable std::atomic<uint64_t> routeAskCovered{ 0 };
    mutable std::atomic<uint64_t> routeAskEscaped{ 0 };

    void NoteRouteAsk(BaseObject* target, bool predicateSaidStale) const
    {
        if (!kRouteAskProbe || target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region == nullptr) {
            return;
        }
        const RegionInfo::RouteState rs = region->GetRouteState();
        if (rs == RegionInfo::RouteState::NORMAL) {
            return;
        }
        if (predicateSaidStale) {
            const uint64_t c = routeAskCovered.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((c & (c - 1)) == 0) {
                // Positive control on the covered arm, so an escaped=0 cannot mean "probe dead".
                LOG(RTLOG_ERROR, "[ROUTEASK] covered=%lu escaped=%lu", c,
                    routeAskEscaped.load(std::memory_order_relaxed));
            }
            return;
        }
        const uint64_t e = routeAskEscaped.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG(RTLOG_ERROR, "[ROUTEASK][ESCAPED] e=%lu covered=%lu target=%p routeState=%d isFrom=%d isGhost=%d fwd=%d",
            e, routeAskCovered.load(std::memory_order_relaxed), static_cast<void*>(target),
            static_cast<int>(rs), IsFromObject(target) ? 1 : 0, IsGhostFromObject(target) ? 1 : 0,
            target->IsForwarded() ? 1 : 0);
    }

    bool IsStaleStoreValue(BaseObject* target) const
    {
        const bool stale = IsFromObject(target) || IsGhostFromObject(target) ||
            (kAskObjectState && target->IsForwarded());
        // PORT_ZFORWARDING step 1: compare the address-keyed answer against the region-keyed one at
        // the busiest place both are meaningful.  Nothing acts on the table yet -- the point is the
        // disagreement count.  legacyOnly > 0 would mean the port loses information and must be
        // fixed before step 2; tableOnly > 0 is the interesting direction, since the table answers
        // from an address and cannot be fooled by a region type that has since moved on.
        if (target != nullptr && Heap::IsHeapAddress(target)) {
            // The baseline has to be the *whole* legacy answer.  Comparing against
            // IsFromObject || IsGhostFromObject alone produced 1.2M tableOnly at rtype=5
            // (UNMOVABLE_FROM_REGION) and 24k at rtype=9 (RAW_POINTER_PINNED_REGION) -- both of
            // which the legacy side does recognise, just through a third predicate.  That was a
            // defect in the comparison, not in the table.
            ForwardingTable::NoteCompare(reinterpret_cast<MAddress>(target),
                                         IsFromObject(target) || IsGhostFromObject(target) ||
                                             IsUnmovableFromObject(target));
        }
        NoteRouteAsk(target, stale);
        return stale;
    }

    // plainroots: root-slot write-back is plain (ZGC uncolored root); heap-slot write-back
    // keeps Phase C colour. Gate MRT_GCV2_PLAIN_ROOTS, on by default.
    //
    // It shipped off for two hours and that was wrong. The stated reason was that a
    // hang carries no si_code, no registers and no core and so is a worse base to
    // debug from -- but hangfloor then diagnosed the hang twice with the gate on,
    // reading per-cycle reclaim out of MRT_GC_LOG. The premise was false, and what
    // the default actually did was let the ledger record a known-wrong behaviour as
    // not-a-blocker.
    //
    // The rule is what ops/design/STACK_ROOTS_STAY_PLAIN.md rules and what ZGC does
    // (zUncoloredRoot.inline.hpp heals a root by writing back zaddress, with the
    // colour passed alongside as an argument). Colour in a register can only have
    // come from the collector's root write-back, which is why the same mechanism
    // surfaced as four separate-looking floor layers. Off means main keeps emitting
    // the defect those four fixes were about.
    //
    // Nothing that works today stops working: hello is 10/10 in all three arms. The
    // arm that hangs was already failing 10/10 as a SEGV.
    static bool PlainRootsEnabled()
    {
        static const bool on = []() {
            const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_PLAIN_ROOTS */;
            if (v == nullptr) {
                return true;
            }
            return !(v[0] == '0' && v[1] == '\0');
        }();
        return on;
    }

    // TRUST_STATE_KILL_PLAN Phase 1 item 2: named overloads replace address-guess wash.
    // HeapSlot → always current colour (Phase C). RootSlot/DerivedSlot → plain when
    // PLAIN_ROOTS on (ZGC uncolored root). Callers must pass the real slot type.
    //
    // nullslot non-heap arm: FixOldTaggedRefField recolour-only path uses the HeapSlot
    // overload (field is a heap RefField). Non-heap *targets* still recolour — never CAS null.
    RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const
    {
        return GetAndTryTagRefField(target);
    }

    RefField<> RootSlotWriteback(BaseObject* target, const RootSlot& /*slot*/) const
    {
        if (PlainRootsEnabled()) {
            return RefField<>(target);
        }
        return GetAndTryTagRefField(target);
    }

    RefField<> RootSlotWriteback(BaseObject* target, const DerivedSlot& /*slot*/) const
    {
        if (PlainRootsEnabled()) {
            return RefField<>(target);
        }
        return GetAndTryTagRefField(target);
    }

    void CollectLargeGarbage()
    {
        MRT_PHASE_TIMER("Collect large garbage");
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

    void DoGarbageCollection() override;
    void ProcessFinalizers() override;
    void EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const override;

private:
    using MinorObjectSet = std::unordered_set<BaseObject*>;
    using MinorRegionSet = std::unordered_set<RegionInfo*>;
    using MinorSlotSet = std::unordered_set<MAddress>;
    using MinorInteriorBaseMap = std::unordered_map<MAddress, BaseObject*>;

    bool MarkObjectImpl(BaseObject* obj, bool youngClaim) const;
    bool MarkYoungObject(BaseObject* obj) const { return MarkObjectImpl(obj, true); }

    // Large reference-array chunking, ported from ZGC's ZMark (zMark.cpp:185-263).
    // `holder` is the owning array, carried only for diagnostics; it is nullptr
    // when the elements arrive as a chunk popped off the work stack, because
    // ZGC's partial-array entry does not carry the holder either.
    void PushPartialArray(RefField<>* addr, size_t length, WorkStack& workStack) const;
    void FollowArrayElementsSmall(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack) const;
    void FollowArrayElementsLarge(BaseObject* holder, RefField<>* addr, size_t length,
                                  WorkStack& workStack) const;
    void FollowArrayElements(BaseObject* holder, RefField<>* addr, size_t length,
                             WorkStack& workStack) const;

    bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, BaseObject* target,
                                  HealSite site, HealNull allowNull = HealNull::Disallow) const;
    BaseObject* ResolveMinorReference(RefField<>& field,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    BaseObject* ResolveMinorReference(RootSlot& root,
                                     const ScopedStopTheWorld* stw = nullptr) const;
    void VisitMinorRootSlots(RootVisitor& rawRootVisitor, uint64_t stackScanEpoch = 0);
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor, uint64_t stackScanEpoch = 0);
    // origin tags root source for invalid-minor-root diagnosis (gcbadroot).
    void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown") const;
    // setbitmap O1③: claim young via MarkObject (region mark bitmap) + collect vector;
    // FYS=0 skips reachableSlots inserts (slots never looked up). MRT_GCV2_SETBITMAP=0 → legacy set.
    // R3 markpar: STW-parallel claim+steal (sibling of ConcurrentMarkingWork); env MARKPAR_*.
    void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                           MinorSlotSet& weakSlots, bool useBitmapLedger,
                           const MinorSlotSet* reachableSlotDomain = nullptr);
    void TraceYoungClosureSerial(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                 std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                 MinorSlotSet& weakSlots, bool useBitmapLedger,
                                 const MinorSlotSet* reachableSlotDomain = nullptr);
    void TraceYoungClosureParallel(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                   std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                   MinorSlotSet& weakSlots, bool useBitmapLedger, GCThreadPool* threadPool,
                                   const MinorSlotSet* reachableSlotDomain = nullptr);
    void TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                  std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                  MinorSlotSet& weakSlots, bool useBitmapLedger, GCThreadPool* threadPool,
                                  const MinorSlotSet* reachableSlotDomain = nullptr);
    // youngconc: drain SATB into TraceYoungClosure (major MarkSatbBuffer sibling; young-only filter).
    bool MarkYoungSatbBuffer(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                             std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                             MinorSlotSet& weakSlots, bool useBitmapLedger,
                             YoungConcWindowStats* windowStats = nullptr);
    friend class YoungMarkingWork;
    friend class YoungStripedMarkingWork;
    void RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                             const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots, bool fullYoungScan,
                             MinorSlotSet* consumedOut = nullptr, DiffPathRemsetStats* statsOut = nullptr,
                             MinorInteriorBaseMap* interiorBasesOut = nullptr,
                             const ScopedStopTheWorld* stw = nullptr);
    bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw = nullptr) const;
    bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr) const;
    void FixMinorRootSlots(const ScopedStopTheWorld* stw = nullptr);
    void FixMinorRootSlotsParallel(GCThreadPool* threadPool, const ScopedStopTheWorld* stw = nullptr);
    void FixMinorObjectSlots(BaseObject* object, const ScopedStopTheWorld* stw = nullptr);
    // stw: live handle lets relocate follow ZGC Phase 7/8 (zGeneration.cpp:573-580):
    // pause = flip + phase + root fix; concurrent = ForwardFromSpace; re-STW = heap
    // slot catch-up + evac_finish. nullptr keeps the whole evacuate under the caller STW.
    void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec, const MinorSlotSet& rememberedSlots,
                              bool refFixSlotsCoveredByReachable, const MinorInteriorBaseMap& interiorBases,
                              std::unique_ptr<ScopedStopTheWorld>* stw = nullptr);
    void ValidateYoungMarking(const std::vector<BaseObject*>& reachableVec, const MinorObjectSet& allocationRoots);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots);
    void ValidateMinorReferences(const char* point, const std::vector<BaseObject*>* reachableVec);
    // Region-set structural verifier (Verify/VerifyRegions); gated by MRT_GCV2_VERIFY_REGIONS.
    void VerifyRegionSets(const char* point);
    void DoYoungGarbageCollection();
    void FlushAllocationRegions();
    template<bool forward>
    bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef) const;
    void TraceHeap();
    // F3: rewrite IsOldPointer slots to plain/to so one-gen-stale tags cannot
    // outlive the route table (REPORT-tagaba F3).
    // requireSurvivedMark=true  → pre-dispel (PostTrace; marks still on from).
    // requireSurvivedMark=false → post-Flip after Forward (to-space has no marks).
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94 + bfb5e8b24fa7c462321709c0c5af8290dccb38a6.
    void InvalidateOldTaggedRefsBeforeDispel();
    void InvalidateOldTaggedRefs(bool requireSurvivedMark);
    void FixOldTaggedRefField(BaseObject* holder, RefField<>& field, const ScopedStopTheWorld& stw);
    void PreforwardConcurrencyModelRoots();
    void PostTrace();
    // OpenJDK ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1523):
    // before old relocate-start flip, remap young roots + remset so none carry
    // two remap-bit errors.
    void RemapYoungRoots();
    void Preforward();
    void PreforwardAllExportFromRoots();
    void PreforwardStaticRoots();
    void PreforwardFinalizerProcessorRoots();
    void PreforwardDiscoveredExternObjects();
    void PreforwardAllResurrectExportFromObjects();
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);

    ForwardTable fwdTable;
    // gc index 0 or 1 is used to distinguish previous gc and current gc.
    uint16_t currentTagID = 0;
    uint64_t minorTotalRuns = 0;
    MinorRegionSet minorCandidateRegions;
};
} // namespace MapleRuntime
#endif // ~MRT_WCOLLECTOR_H
