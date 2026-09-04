// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Common/ColourMask.h"
#include "Base/TimeUtils.h"
#include "Heap/Verify/HealCoverage.h"
#include "Heap/WCollector/RemapYoungRoots.h"
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
#include "Heap/Allocator/ForwardingTable.h"
#include "Collector/CopyCollector.h"
#include "Heap/Collector/RemsetScanStats.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
class MarkLiveCache;
class ScopedStopTheWorld;

// paramzero: crash-time dump of Mode-A frame slot + heap CAS-null counters.
// Declared here so SignalManager can call without including WCollector.cpp guts.
// Gate = MRT_GCV2_NULLSLOT (default off).
void EmitParamzeroCrashProbe(uintptr_t rbp, uintptr_t rbx, uintptr_t rip);

#if defined(MRT_TESTABLE_INTERNALS)
// One-shot wave8 attribution receipts.  The storage is native and fixed-size;
// production builds do not declare or emit any of this instrumentation.
enum class RemsetFilterReceiptReason : uint8_t {
    kNone = 0,
    kStale = 1,
    kDeadHolder = 2,
    kNoOrigin = 3,
    kBadTarget = 4,
};
struct RemsetFilterTestReceipt {
    uint64_t seen = 0;
    uint64_t consumed = 0;
    uint64_t stale = 0;
    uint64_t deadHolder = 0;
    uint64_t noOrigin = 0;
    uint64_t badTarget = 0;
    MAddress lastConsumedSlot = 0;
    MAddress lastStaleSlot = 0;
    MAddress lastDeadHolderSlot = 0;
    MAddress lastNoOriginSlot = 0;
    MAddress lastBadTargetSlot = 0;
};
void ResetRemsetFilterTestReceipt();
RemsetFilterTestReceipt ReadRemsetFilterTestReceipt();
void NoteRemsetFilterTestReceipt(MAddress slot, RemsetFilterReceiptReason reason, bool consumed);

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
void ArmSatbBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second);
void PublishSatbBeforeMarkEndTestReceipt();

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
// non-zero windowNs with markedInWindow()==0 and satbObjects==0 means the
// window carries no marking work; do not read duration alone as concurrency.
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

#if defined(MRT_TESTABLE_INTERNALS)
enum class YoungWeakClosureVariant : uint8_t {
    SERIAL,
    LEGACY_PARALLEL,
    STRIPED,
};

struct YoungWeakClosureTestReceipt {
    uint64_t serial = 0;
    uint64_t legacyParallel = 0;
    uint64_t striped = 0;
};

void ResetYoungWeakClosureTestReceipt();
void NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant variant);
YoungWeakClosureTestReceipt ReadYoungWeakClosureTestReceipt();
#endif

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
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MutatorPublishTestAccess;
    friend struct PartialArrayTestAccess;
    friend struct RelocationReceiptTestAccess;
    friend struct RemsetRearmTestAccess;
    friend struct LoadHealDeliveryTestAccess;
#endif

public:
    explicit WCollector(Allocator& allocator, CollectorResources& resources)
        : CopyCollector(allocator, resources), fwdTable(reinterpret_cast<RegionSpace&>(allocator))
    {
        collectorType = CollectorType::SMOOTH_COLLECTOR;
    }

    ~WCollector() override = default;

#if defined(MRT_GC_UNIT_TESTS)
    // Controlled test wrapper for the copier route consumer. It keeps the
    // consumer's preconditions visible (heap address, relocation phase and
    // read retain) while using the same CopierRouteToken mint as production.
    struct RouteLookupTestResult {
        RoutePlan plan{};
        bool phaseAllowed = false;
        bool heapAddress = false;
        bool retained = false;
        bool gatePassed = false;
        bool receiptChecked = false;
        bool compactedChecked = false;
        bool routeRegionCalled = false;
        bool routeRegion = false;
        bool retainedPhaseAllowed = false;
        bool hookReached = false;
    };
    MRT_EXPORT RouteLookupTestResult PlanRouteLookupForTest(BaseObject* fromObj);
#endif

    void Init() override { ForwardDataManager::GetForwardDataManager().InitializeForwardData(); }

    void MarkNewObject(BaseObject* obj) override;

    bool ShouldIgnoreRequest(GCRequest& request) override;
    bool MarkObject(BaseObject* obj) const override;
    bool ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* regionInfo) override;

    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void TraceRefField(BaseObject* obj, RefField<>& ref, WorkStack& workStack) const;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack) override;
    void FollowPartialArray(const MarkStackEntry& entry, WorkStack& workStack) override;
    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field) override;
    BaseObject* ForwardObject(BaseObject* fromVersion) override;
    BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance = {}) const override;
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
    // the static initialisers in BaseObject.cpp stop being two independent copies of the
    // same formula. The positive LoadGood word is published next to LoadBad
    // (zAddress.cpp:81,85), and StoreGood next to StoreBad (:83,87); the four
    // flip_* functions still all funnel through this one writer.
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
        ::g_cjLoadGoodMask = m.remapColour;
        ::g_cjLoadBadMask = m.loadBad;
        ::g_cjMarkBadMask = m.markBad;
        ::g_cjStoreBadMask = m.storeBad;
        ::g_cjStoreGoodMask = m.storeGood;
        // zAddress.cpp:87 StoreBad = StoreGood ^ StoreMetadataMask. Checked on every
        // publication so the two exported words cannot drift (ColourMask.h ComputeBadMasks
        // is the only writer; this CHECK is the runtime witness of that identity).
        CHECK((m.storeGood ^ STORE_METADATA_MASK) == m.storeBad);
        CHECK(::g_cjLoadGoodMask == m.remapColour);
        CHECK((::g_cjLoadGoodMask | ::g_cjLoadBadMask) ==
              (REMAP_COLOUR_MASK | TAGGED_BITS_MASK));
        CHECK((::g_cjLoadGoodMask & ::g_cjLoadBadMask) == 0);
        CHECK(::g_cjStoreGoodMask == m.storeGood);
        CHECK(::g_cjStoreBadMask == m.storeBad);
    }

    // OpenJDK ZGlobalsPointers::flip_young_relocate_start/flip_old_relocate_start
    // (zAddress.cpp:138-151): each generation independently alternates the two accepted pairs.
    void flip_young_relocate_start()
    {
        ZPointerRemappedYoungMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
        // Heal coverage before colour reuse (zGeneration.cpp:1503-1508).
        // Gate is a compile-time constant so the product rec=stw arm pays no walk.
        if (HealCoverage::kHealCoverageCensus) {
            HealCoverage::CensusAfterPublication(
                currentRemapColour, FlipSeq().load(std::memory_order_relaxed), "flip-young");
        }
    }

    void flip_old_relocate_start()
    {
        ZPointerRemappedOldMask ^= REMAP_COLOUR_MASK;
        set_good_masks();
        if (HealCoverage::kHealCoverageCensus) {
            HealCoverage::CensusAfterPublication(
                currentRemapColour, FlipSeq().load(std::memory_order_relaxed), "flip-old");
        }
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
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const override
    {
        if (!Heap::IsHeapAddress(obj)) {
            return obj;
        }
        const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
        RegionInfo* forwarding = RegionInfo::GetGhostFromRegionAt(fromAddr);
        if (forwarding == nullptr || forwarding->generation_id() != generation) {
            const MAddress retired = ForwardingTable::FindRetiredTo(fromAddr);
            if (retired != 0) {
                BaseObject* to = reinterpret_cast<BaseObject*>(retired);
                if (ToHeaderCovered(to)) {
                    return to;
                }
            }
            return obj;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        const bool entriesArmed = ForwardingTable::EntriesArmed(fromAddr);
        if constexpr (ForwardingTable::kEntriesSoleWhenArmed) {
            if (entriesArmed) {
                const MAddress stored = ForwardingTable::FindTo(fromAddr);
                if (stored != 0) {
                    BaseObject* to = reinterpret_cast<BaseObject*>(stored);
                    if (ToHeaderCovered(to)) {
                        return to;
                    }
                }
                if (!obj->IsForwarded()) {
                    // Armed miss on a not-yet-copied object: do not invent geometry.
                }
            }
        }
        if (obj->IsForwarded()) {
            BaseObject* published = GetForwardPointer(obj, forwarding);
            if (published != nullptr) {
                return published;
            }
        }
        BaseObject* to = space.GetRegionManager().FindPublishedRoute(obj, forwarding).dest;
        if constexpr (ForwardingTable::kEntriesSoleWhenArmed) {
            if (entriesArmed && !obj->IsForwarded()) {
                to = nullptr;
            }
        }
        if (to != nullptr) {
            if (LIKELY(!Heap::IsHeapAddress(to))) {
                return to;
            }
            if ((obj->IsForwarded() || forwarding->IsCompacted()) && to->IsValidObject()) {
                return to;
            }
        } else if (obj->IsForwarded()) {
            BaseObject* published =
                FindToVersion(obj).GetOrFailClosed("WCollector::ForwardObjectImpl");
            if (published != nullptr) {
                return published;
            }
        }
        // ② retain + copy (zRelocate.cpp:393-400). nullptr = retain refused or copy missed.
        // The ZGC slow path has one relocate_object invocation per visitor.
        // TryMutatorRelocate already owns the retain token when it calls back
        // into ForwardObjectImpl; re-entering it here would recursively retain
        // the same page until the token is refused and lose the first-visitor
        // publication opportunity (zBarrier.inline.hpp:294-343).
        BaseObject* self = MutatorRelocate::InScope() ? nullptr : TryMutatorRelocate(obj, forwarding);
        if (self != nullptr) {
            return self;
        }
        to = space.GetRegionManager().FindPublishedRoute(obj, forwarding).dest;
        if (to != nullptr && Heap::IsHeapAddress(to) && to->IsValidObject()) {
            return to;
        }
        // ③ table still empty. Wait for the region-level publication while a
        // copier exists. A completed publication without a receipt is an
        // invariant failure, never a from-address answer (zRelocate.cpp:382-416).
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitEnter();
        }
        BaseObject* resolved = WaitRoutedTipReady(obj, to, forwarding);
        if (resolved != nullptr && resolved != obj) {
            return resolved;
        }
        // inplaceto: on a page compacted in place, WaitRoutedTipReady answers `obj` only when the
        // page geometry classifies this address as already relocated (ClassifyCompactedMiss
        // kAlreadyTo), or when the table holds an identity receipt for it.  Both say "obj is the
        // current version", which is what ZRelocate::forward_object returns when find() maps an
        // object onto itself (zRelocate.cpp:411-415 asserts an answer exists, not that it moved).
        // The `resolved != obj` guard above still refuses a from-address on every other page.
        if (resolved == obj && forwarding != nullptr && forwarding->IsCompacted()) {
            return obj;
        }
        CHECK_DETAIL(false,
                     "ZRelocate::forward_object requires a forwarding entry for relocation-set object %p",
                     obj);
        return nullptr;
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
        if (obj != nullptr) {
            RegionInfo* ghost = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
            if (ghost != nullptr && !ghost->IsUnmovableFromRegion()) {
                const GCPhase p = GetGCPhase();
                if (p == GCPhase::GC_PHASE_PREFORWARD || p == GCPhase::GC_PHASE_FORWARD) {
                    obj = ResolveStoreValue(obj);
                }
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
    static constexpr bool kMembershipFromTable = ForwardingTable::kZfwdTableConsume;

    bool IsFromObject(BaseObject* obj) const override
    {
        if (kMembershipFromTable) {
            if (!Heap::IsHeapAddress(obj)) {
                return false;
            }
            if (ForwardingTable::Get(reinterpret_cast<MAddress>(obj)) == nullptr) {
                return false;
            }
            // The table is installed over a heap span; membership still
            // requires the per-region forwarding publication.  Unselected
            // regions retain a NORMAL route and no forwarding face, and must
            // not be classified as relocation-set addresses merely because
            // their address falls inside that span (zGeneration.cpp:254).
            RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
            return region != nullptr &&
                (region->GetLiveInfo0ForProbe() != nullptr ||
                 region->GetRouteState() != RegionInfo::RouteState::NORMAL ||
                 region->IsForwardingDone());
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

    // zRelocate.cpp:368-372: insert publishes a completed copy only. A geometric
    // GetRoute dest or table hit with TypeInfo=0 is not a find() hit.
    static bool ToHeaderCovered(BaseObject* to)
    {
        return to != nullptr && Collector::PlausibleManagedObjectGate("ToHeaderCovered", to);
    }

    BaseObject* GetForwardPointer(BaseObject* fromObj, RegionInfo* region) const
    {
        const MAddress fromAddr = reinterpret_cast<MAddress>(fromObj);
        BaseObject* to = nullptr;
        if constexpr (ForwardingTable::kEntriesSoleWhenArmed) {
            if (ForwardingTable::EntriesArmed(fromAddr)) {
                const MAddress stored = ForwardingTable::FindTo(fromAddr);
                to = stored == 0 ? nullptr : reinterpret_cast<BaseObject*>(stored);
                if (ToHeaderCovered(to)) {
                    return to;
                }
                if (!fromObj->IsForwarded()) {
                    return nullptr;
                }
            }
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        to = space.GetRegionManager().FindPublishedRoute(fromObj, region).dest;
        if (!fromObj->IsForwarded() || !ToHeaderCovered(to) ||
            !ZForwarding::DestUsable(reinterpret_cast<MAddress>(to))) {
            return nullptr;
        }
        ForwardingTable::Publication publication =
            ForwardingTable::RetainOpenPublicationAfterCopy(region, fromAddr);
        if (!publication) {
            return nullptr;
        }
        const MAddress receipt = ForwardingTable::InsertMapping(
            publication, fromAddr, reinterpret_cast<MAddress>(to));
        (void)space.GetRegionManager().GetRelocationRequestQueue().Publish(fromAddr, receipt);
        return reinterpret_cast<BaseObject*>(receipt);
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

    FindToVersionResult FindToVersion(BaseObject* obj) const override
    {
        // Mirror IsGhostFromObject: GetGhostFromRegionAt → GetUnitIdxAt has no heap range
        // check, so null / non-heap (incl. colour-only null after flip) aborts as
        // "GetUnitIdxAt OOB addr=0". FixOldTaggedRefField then nulls the slot.
        // nullptr here is dual: non-heap/null gate OR unpublished / no to-version.
        // Soft-resolve paths must not CAS-null on the non-heap reading (RO static).
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return FindToVersionResult::NotManaged();
        }
        const MAddress fromAddr = reinterpret_cast<MAddress>(obj);
        const auto answerName = [](ForwardingTable::ToAnswer answer) -> const char* {
            switch (answer) {
                case ForwardingTable::ToAnswer::ArmedHit:
                    return "armed_hit";
                case ForwardingTable::ToAnswer::ArmedMiss:
                    return "armed_miss";
                case ForwardingTable::ToAnswer::Unavailable:
                    return "unavailable";
                case ForwardingTable::ToAnswer::Unarmed:
                    return "unarmed";
            }
            return "unknown";
        };
        const auto causeName = [](ForwardingTable::ToUnavailableCause cause) -> const char* {
            switch (static_cast<uint8_t>(cause)) {
                case 1:
                    return "active_retain_rejected";
                case 2:
                    return "retired_unavailable";
                case 3:
                    return "active_retain_rejected+retired_unavailable";
                case 4:
                    return "publication_closed";
                case 5:
                    return "active_retain_rejected+publication_closed";
                case 6:
                    return "retired_unavailable+publication_closed";
                case 7:
                    return "active_retain_rejected+retired_unavailable+publication_closed";
                case 8:
                    return "table_destroyed";
                case 12:
                    return "publication_closed+table_destroyed";
                case 16:
                    return "never_installed";
                case 20:
                    return "publication_closed+never_installed";
                case 0:
                    return "none";
            }
            return "unknown";
        };
        ForwardingTable::LookupResult lookup{ 0, ForwardingTable::ToAnswer::Unarmed,
                                              ForwardingTable::ToUnavailableCause::None, false, false,
                                              ForwardingTable::ToAnswer::Unarmed,
                                              ForwardingTable::ToAnswer::Unarmed, false, false, 0 };
        bool lookupQueried = false;
        const auto populateDiagnosticSnapshot = [&](FindToVersionResult::UnavailableWitness& witness) {
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(fromAddr);
            witness.from = fromAddr;
            witness.fromRegion = reinterpret_cast<uintptr_t>(region);
            witness.regionSnapshotValid = region != nullptr;
            if (region != nullptr) {
                witness.regionType = static_cast<uint8_t>(region->GetRegionType());
                witness.generation = static_cast<uint8_t>(region->generation_id());
                witness.routeStateValid = true;
                witness.routeState = static_cast<uint8_t>(region->GetRouteState());
            }
            witness.inCurrentRelocationSet = lookupQueried && lookup.currentMembership;
            witness.tableId = lookupQueried ? lookup.tableId : 0;
            witness.gcPhase = static_cast<uint8_t>(GetGCPhase());
        };
        const auto unavailable = [&](FindToVersionResult::UnavailableRoute route, bool forwardedValid,
                                     bool forwarded, bool fromRegionInfoNullValid,
                                     bool fromRegionInfoNull) -> FindToVersionResult {
            FindToVersionResult::UnavailableWitness witness;
            witness.forwardedValid = forwardedValid;
            witness.forwarded = forwarded;
            witness.fromRegionInfoNullValid = fromRegionInfoNullValid;
            witness.fromRegionInfoNull = fromRegionInfoNull;
            populateDiagnosticSnapshot(witness);
            // All lookup fields are a single snapshot.  When LookupTo was not
            // reached (the legacy compile-time route), leave the snapshot
            // invalid so consumers print n/a instead of defaults.
            witness.lookupSnapshotValid = lookupQueried;
            if (lookupQueried) {
                witness.lookupAnswer = answerName(lookup.answer);
                witness.lookupCause = causeName(lookup.unavailableCause);
                witness.lookupActiveCandidate = lookup.activeCandidate;
                witness.lookupActiveAnswer = answerName(lookup.activeAnswer);
                witness.lookupRetiredAnswer = answerName(lookup.retiredAnswer);
                witness.lookupPublicationClosed = lookup.publicationClosed;
            }
            return FindToVersionResult::Unavailable(route, witness);
        };
        BaseObject* stored = nullptr;
        if constexpr (ForwardingTable::kConsumeEntries) {
            lookup = ForwardingTable::LookupTo(fromAddr);
            lookupQueried = true;
            if (lookup.to != 0) {
                stored = reinterpret_cast<BaseObject*>(lookup.to);
            }
            if constexpr (ForwardingTable::kEntriesSoleWhenArmed) {
                if (lookup.answer == ForwardingTable::ToAnswer::ArmedHit) {
                    return ToHeaderCovered(stored) ? FindToVersionResult::Found(stored)
                                                  : FindToVersionResult::NotForwarded();
                }
                if (lookup.answer == ForwardingTable::ToAnswer::Unavailable) {
                    const uint8_t causeBits = static_cast<uint8_t>(lookup.unavailableCause);
                    // zGeneration.inline.hpp:131-135: forwarding table gone → addr.
                    if ((causeBits & static_cast<uint8_t>(
                            ForwardingTable::ToUnavailableCause::TableDestroyed)) != 0 &&
                        Collector::JudgeHandOutTarget(obj) == HandVerdict::Usable &&
                        !obj->IsForwarded()) {
                        return FindToVersionResult::NotForwarded();
                    }
                    FindToVersionResult::UnavailableWitness witness;
                    witness.lookupAnswer = answerName(lookup.answer);
                    witness.lookupSnapshotValid = true;
                    witness.lookupCause = causeName(lookup.unavailableCause);
                    witness.lookupActiveCandidate = lookup.activeCandidate;
                    witness.lookupActiveAnswer = answerName(lookup.activeAnswer);
                    witness.lookupRetiredAnswer = answerName(lookup.retiredAnswer);
                    witness.lookupPublicationClosed = lookup.publicationClosed;
                    populateDiagnosticSnapshot(witness);
                    return FindToVersionResult::Unavailable(
                        FindToVersionResult::UnavailableRoute::LookupUnavailable, witness);
                }
                if (lookup.answer == ForwardingTable::ToAnswer::ArmedMiss && !obj->IsForwarded()) {
                    return FindToVersionResult::NotForwarded();
                }
            }
        }
        if (stored != nullptr && !ToHeaderCovered(stored)) {
            stored = nullptr;
        }
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(fromAddr);
        if (fromRegionInfo == nullptr) {
            if (stored != nullptr) {
                return FindToVersionResult::Found(stored);
            }
            const bool forwarded = obj->IsForwarded();
            return forwarded
                ? unavailable(FindToVersionResult::UnavailableRoute::NoGhostForwarded,
                              true, forwarded, true, true)
                : FindToVersionResult::NotForwarded();
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        BaseObject* geometric = space.GetRegionManager().FindPublishedRoute(obj).dest;
        if (geometric != nullptr) {
            ForwardingTable::NoteDestCompare(fromAddr, reinterpret_cast<MAddress>(geometric));
        }
        if (geometric != nullptr && ZForwarding::DestUsable(reinterpret_cast<MAddress>(geometric)) &&
            ToHeaderCovered(geometric)) {
            if (stored == nullptr) {
                ForwardingTable::Publication publication =
                    ForwardingTable::RetainOpenPublicationAfterCopy(fromRegionInfo, fromAddr);
                if (!publication) {
                    return unavailable(
                        FindToVersionResult::UnavailableRoute::PublicationRetainFailed,
                        false, false, true, false);
                }
                const MAddress receipt = ForwardingTable::InsertMapping(
                    publication, fromAddr, reinterpret_cast<MAddress>(geometric));
                (void)space.GetRegionManager().GetRelocationRequestQueue().Publish(fromAddr, receipt);
                geometric = reinterpret_cast<BaseObject*>(receipt);
            }
            return FindToVersionResult::Found(geometric);
        }
        if constexpr (ForwardingTable::kConsumeEntries) {
            if (stored != nullptr) {
                return FindToVersionResult::Found(stored);
            }
            const bool forwarded = obj->IsForwarded();
            return forwarded
                ? unavailable(FindToVersionResult::UnavailableRoute::GeometricMissForwarded,
                              true, forwarded, true, false)
                : FindToVersionResult::NotForwarded();
        }
        if (geometric != nullptr && ToHeaderCovered(geometric)) {
            return FindToVersionResult::Found(geometric);
        }
        const bool forwarded = obj->IsForwarded();
        return forwarded
            ? unavailable(FindToVersionResult::UnavailableRoute::LegacyGeometricMiss,
                          true, forwarded, true, false)
            : FindToVersionResult::NotForwarded();
    }

protected:
    BaseObject* ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion);
    BaseObject* ForwardObjectExclusive(BaseObject* obj) override;
    // dest is PlanRoute's answer, computed *before* TryLockObject so the LOCKED
    // critical section cannot RouteRegion / TakeRegion (zRelocate.cpp:354-372
    // relocate_object_inner: alloc+copy+insert, no safepoint; REPORT-routespin §5 乙1).
    // copyPage is the from-page NoteCopyInflight already ran on (TryLock success);
    // Exclusive only EndCopyInflight after every UnlockObject.
    BaseObject* ForwardObjectExclusive(BaseObject* obj, BaseObject* toObj, RegionInfo* copyPage);

    // Wait until a forwarding receipt is published; completed miss is an
    // invariant failure (zRelocate.cpp:382-416).
    BaseObject* WaitRoutedTipReady(BaseObject* from, BaseObject* to, RegionInfo* forwarding) const;

    // portmutreloc: ZRelocate::relocate_object's middle leg (zRelocate.cpp:391-406) --
    // retain the from-region, relocate the object on this thread, release. Returns the
    // to-version, or nullptr when the owning copier must supply the receipt.
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

    // Produce the load-bad member of the full-colour family without changing
    // the address.  This is deliberately separate from GetAndTryTagRefField:
    // unclassified store values still have to pass ResolveStoreValue first.
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
    RefField<> ColourResolvedRefField(BaseObject* target) const
    {
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        if (!Heap::IsHeapAddress(target)) {
            return RefField<>(target);
        }
        CHECK_DETAIL(Collector::JudgeHandOutTarget(target) == HandVerdict::Usable,
                     "store-good requires a usable resolved address target=%p", target);
        CHECK_DETAIL(!IsStaleStoreValue(target),
                     "store-good must not colour a relocation-set address target=%p", target);
        if (kColourWhoProbe) {
            NoteColourStoreGoodOnBadTarget(target);
        }
        return ColourStoreGood(from_object(target));
    }

    RefField<> GetAndTryTagRefField(BaseObject* target) const override
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad).
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        // TypeInfo* / binary constants / immortal metadata are not relocated,
        // but a non-null HeapSlot word is still coloured.  The load-good mask
        // fast path peels it without routing through the collector.
        if (!Heap::IsHeapAddress(target)) {
            return ColourStoreGood(from_object(target));
        }
        // ZPointer::uncolor is the sole producer accepted by ZAddress::store_good
        // (zAddress.inline.hpp:609-624,806-811). ResolveStoreValue is our
        // make-load-good producer: a relocation-set address is looked up or copied
        // by this thread; an unresolved address never reaches colouring.
        target = ResolveStoreValue(target);
        CHECK_DETAIL(target != nullptr && Heap::IsHeapAddress(target),
                     "store-good requires a resolved heap address");
        CHECK_DETAIL(Collector::JudgeHandOutTarget(target) == HandVerdict::Usable,
                     "store-good requires a usable resolved address target=%p", target);
        CHECK_DETAIL(!IsStaleStoreValue(target),
                     "store-good must not colour a relocation-set address target=%p", target);
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
        // FindToVersion / PlanRouteUnderStw / IsFromObject / IsGhostFromObject all
        // refuse a non-heap address.  kAskObjectState used to read
        // target->IsForwarded() (StateWord objectState at +6) with no heap gate.
        // GetAndTryTagRefField is handed TypeInfo* / binary constants / immortal
        // metadata after Flip (PlanRouteUnderStw:611-613).  cjpm N=5 r1 on
        // 1f8730a54: target=0x646e65706564 ASCII "depend", si_addr=target+6,
        // insn=movzx 0x6(%r12),%eax @ IsStaleStoreValue, forward/fix.
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return false;
        }
        RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
        if (region != nullptr &&
            region->IsCompactRouteDestination(reinterpret_cast<MAddress>(target)) &&
            Collector::JudgeHandOutTarget(target) == HandVerdict::Usable) {
            // A completed compact-in-place route is positive to-address
            // provenance. ZGC exposes the reset page only after every
            // forwarding entry has been installed (zRelocate.cpp:1013-1037).
            return false;
        }
        FindToVersionResult mapped = FindToVersion(target);
        if (mapped.found() == target && Collector::JudgeHandOutTarget(target) == HandVerdict::Usable) {
            // ZGC's allocation-failure path can relocate a page in place, but
            // still inserts one forwarding entry per live object before
            // detach_page (zRelocate.cpp:862-925,1013-1037).
            return false;
        }
        // Membership is "this page will move", not "this address has moved", and only the second
        // is staleness.  ZGeneration::relocate_or_remap_object hands every member address to
        // ZRelocate::relocate_object, whose first act is forwarding->find(from_addr): a hit means
        // the object already moved, so the from-address is stale; a miss means it has not moved,
        // and relocate_object copies it and returns the new address
        // (zGeneration.inline.hpp:131-140; zRelocate.cpp:382-416).  A page enters the relocation
        // set full of live objects whose own addresses are still the only correct ones, so ZGC has
        // no predicate anywhere that reads membership as staleness, and cannot have one.
        //
        // The leg deleted here was `region != nullptr || IsFromObject(target)`.  Measured on
        // NW256/256MB, verbatim in the two shots that reached the refusal (SO ea3c92943e1714a6 and
        // 965d7c2417a721b9): the refused address had entriesArmed=1 with lookupTo=0
        // toAnswer=ArmedMiss, header stateCode=0 and FindToVersion=NotForwarded -- an object
        // nothing had moved -- while the producer that had just handed it back recorded
        // resolveExit=4 (a forwarding-table hit) with exitTableArmed=0 exitGhost=0.  The
        // relocation-set enrolment walk ran on another thread in between.  A predicate whose
        // answer flips under a concurrent enrolment is not an invariant of the value.
        //
        // The two provenance exits above stay: under compact-in-place the from- and to-layouts
        // share one span, so the same address can be both a published destination and a from-index
        // that the table maps elsewhere, and only the page's own record separates them
        // (zForwarding.cpp:55-64).  Dropping them made a compacted-page destination read as stale
        // 3/3 at gc=0 (SO 12e241ab16cb4bd1, from=resolved=regionStart+33480).
        //
        // The "has moved" answer has to come from the *current* forwarding for this address, not
        // from FindToVersion: that one falls back to a retired-table scan, and ZGC has no such
        // tail -- a ZForwarding exists only for the relocation that created it and is detached at
        // its end (zForwarding.cpp:171-181).  Under compact-in-place a retired entry indexes a
        // live current from-address and answers with the destination some other object was moved
        // to a cycle ago (ForwardingTable.cpp retiredserve).  Measured 2/2 at gc=0 on SO
        // 342125e11f1d5fc6: a NORMAL, non-member region (route=0 ghost=nil isFrom=0
        // entriesArmed=0) whose retired table mapped regionStart+8968 onto regionStart+4856 --
        // the address was current and the answer a cycle old.
        const bool unmovable = IsUnmovableFromObject(target);
        const MAddress targetAddr = reinterpret_cast<MAddress>(target);
        MAddress publishedAddr = 0;
        if (ZForwarding* entries = ForwardingTable::GetEntries(targetAddr)) {
            publishedAddr = entries->find(targetAddr);
        }
        const bool stale = !unmovable &&
            ((publishedAddr != 0 && publishedAddr != targetAddr) ||
             (kAskObjectState && target->IsForwarded()));
        NoteRouteAsk(target, stale);
        return stale;
    }

    // Typed, unconditional boundary: HeapSlot write-back is coloured while
    // stack/register/static RootSlot write-back stays plain, matching ZGC's
    // ZUncoloredRoot.  There is no runtime switch between the two contracts.
    RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const
    {
        return GetAndTryTagRefField(target);
    }

    RefField<> RootSlotWriteback(BaseObject* target, const RootSlot& /*slot*/) const
    {
        return RefField<>(target);
    }

    bool HealRootWriteback(RootSlot& root, BaseObject* target, HealSite site,
                           std::memory_order order = std::memory_order_relaxed) const
    {
        RefField<> desired = RootSlotWriteback(target, root);
        root.StoreCollectorRollback(desired.GetFieldValue(), order);
        (void)site;
        return true;
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

    bool MarkObjectImpl(BaseObject* obj, bool youngClaim, MarkLiveCache* liveCache = nullptr) const;
    bool MarkYoungObject(BaseObject* obj, MarkLiveCache* liveCache = nullptr) const
    {
        return MarkObjectImpl(obj, true, liveCache);
    }

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

    bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                  HealSite site, HealNull allowNull = HealNull::Disallow) const;
    BaseObject* ResolveMinorReference(RefField<>& field,
                                     const ScopedStopTheWorld* stw = nullptr,
                                     bool holderIsCurrentMinorRoot = false,
                                     bool* preservedByCurrentRoot = nullptr) const;
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
    // ZMark::try_end sibling: called with mutators stopped; performs exactly one
    // local-buffer flush and reports whether concurrent-mark-continue is needed.
    bool TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats = nullptr);
    friend class YoungMarkingWork;
    friend class YoungStripedMarkingWork;
    void RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                             const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                             const MinorObjectSet& currentMinorRoots, bool fullYoungScan,
                             MinorSlotSet* consumedOut = nullptr, RemsetScanStats* statsOut = nullptr,
                             MinorInteriorBaseMap* interiorBasesOut = nullptr,
                             const ScopedStopTheWorld* stw = nullptr);
    bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,
                               const ScopedStopTheWorld* stw = nullptr,
                               bool holderIsCurrentMinorRoot = false) const;
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
                              const MinorObjectSet& currentMinorRoots, bool refFixSlotsCoveredByReachable,
                              const MinorInteriorBaseMap& interiorBases,
                              std::unique_ptr<ScopedStopTheWorld>* stw = nullptr);
    void ValidateYoungMarking(const std::vector<BaseObject*>& reachableVec, const MinorObjectSet& allocationRoots);
    // Report-only: find young objs full-reachable but unmarked; attribute via remset MISSING.
    // Gated by MRT_GCMARKGAP_PROBE=1 (default off).
    void ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots);
    void ValidateMinorReferences(const char* point, const std::vector<BaseObject*>* reachableVec);
    // Region-set structural verifier (Verify/VerifyRegions); gated by MRT_GCV2_VERIFY_REGIONS.
    void VerifyRegionSets(const char* point);
    void DoYoungGarbageCollection();
    // After nested young, remaining young survivors hold young→old edges the
    // young closure skipped. ZGC overlapping mark paints old targets from those
    // stores (zBarrier.inline.hpp:742-749). Seed them into the old TRACE stack.
    void SeedOldMarkFromYoungSurvivors(WorkStack& workStack, std::vector<BaseObject*>* collectOnly);
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
