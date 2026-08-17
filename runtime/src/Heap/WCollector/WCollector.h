// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WCOLLECTOR_H
#define MRT_WCOLLECTOR_H
#include "Common/ColourMask.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "Allocator/RegionSpace.h"
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

    // if object is not relocated (forwarded or compacted), return nullptr.
    BaseObject* RouteObject(BaseObject* old)
    {
        BaseObject* toAddress = theSpace.RouteObject(old);
        return toAddress;
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
    void set_good_masks()
    {
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
    // permhole receiptization: RouteObject is geometric (ROUTED before Copy fills tip).
    // Receipt is from-FORWARDED (copy + release already happened), not tip-valid.
    // IsValidObject is only a hole check after FORWARDED. Mid-copy returns to Wait.
    // WaitRoutedTipReady returns receipt, or from if copy has not started, or CHECKs hole.
    // permhit: WaitRoutedTipReady is reachable from here and nowhere else (Collector.h:169
    // make_load_good is its only caller), so "enter=0" alone cannot say whether the wait was
    // never needed or whether the read barrier never reached this funnel at all. Count each
    // arm under the same MRT_GCV2_WAITFWD gate; every counter is behind the gate, so the
    // product path is unchanged when it is off.
    static bool RemapFunnelOn()
    {
        static const int on = []() {
            const char* v = std::getenv("MRT_GCV2_WAITFWD");
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
                                 "routeNull=%llu receipt=%llu wait=%llu\n",
                                 static_cast<unsigned long long>(callCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(nonHeapCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(noGhostCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(routeNullCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(receiptCount.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(waitCount.load(std::memory_order_relaxed)));
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
        BaseObject* to = space.GetRegionManager().RouteObject(obj, forwarding);
        if (to == nullptr) {
            // ZRelocate::forward_object after retain_page refused (zRelocate.cpp:408-410):
            // the page is done; the object must already be in the forwarding table.
            if (obj->IsForwarded()) {
                BaseObject* published = FindToVersion(obj);
                if (published != nullptr) {
                    if (funnel) {
                        receiptCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    return published;
                }
            }
            if (funnel) {
                routeNullCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (tv) {
                ToverFailDiag::NoteRemapRouteNull(); // 甲
            }
            return obj;
        }
        if (LIKELY(!Heap::IsHeapAddress(to))) {
            if (funnel) {
                receiptCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (tv) {
                ToverFailDiag::NoteRemapReceipt();
            }
            return to;
        }
        if (obj->IsForwarded() || forwarding->IsCompacted()) {
            if (to->IsValidObject()) {
                if (funnel) {
                    receiptCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (tv) {
                    ToverFailDiag::NoteRemapReceipt();
                }
                return to;
            }
        } else {
            // portmutreloc: this is the branch ZRelocate::relocate_object reaches when find()
            // came back null (zRelocate.cpp:386-406) -- a route exists, no to-version has been
            // installed yet -- and it is where ZGC stops deferring to a worker:
            //
            //     if (forwarding->retain_page(&_queue)) {
            //         to_addr = relocate_object_inner(forwarding, safe(from_addr), &cursor);
            //         forwarding->release_page();
            //         if (!is_null(to_addr)) return to_addr;
            //         _queue.add_and_wait(forwarding);
            //     }
            //
            // Everything below was the "else" of that: hand the mutator the *from* pointer
            // back when no copy has started, otherwise spin in WaitRoutedTipReady up to
            // kMaxSpins=4096 and FATAL. Both legs are kept -- this is an added first choice,
            // not a replacement.
            BaseObject* self = TryMutatorRelocate(obj, forwarding);
            if (self != nullptr) {
                return self;
            }
            // ZRelocate::relocate_object after retain/copy refused: add_and_wait,
            // never hand the from address back (zRelocate.cpp:403-409). Returning
            // from here is what made survival_dense checksum drift under concurrent
            // relocate — the mutator read a from-face that flip had already made
            // load-bad, then treated the id as live.
        }
        if (funnel) {
            waitCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (tv) {
            ToverFailDiag::NoteRemapWait(); // 丙 entry
        }
        // Gated on StatsOn(), not on the feature: this is the leg the port exists to displace,
        // so it must be counted with the feature OFF too or the on-arm has nothing to be
        // compared against.
        if (MutatorRelocate::StatsOn()) {
            MutatorRelocate::NoteWaitEnter();
        }
        return WaitRoutedTipReady(obj, to, forwarding);
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

    bool IsFromObject(BaseObject* obj) const override
    {
        // filter const string object.
        if (Heap::IsHeapAddress(obj)) {
            auto regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
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

    // this is called when caller assures from-object/from-region still exists.
    BaseObject* GetForwardPointer(BaseObject* fromObj, RegionInfo* region)
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().RouteObject(fromObj, region);
    }

    BaseObject* FindToVersion(BaseObject* obj) const override
    {
        // Mirror IsGhostFromObject: GetGhostFromRegionAt → GetUnitIdxAt has no heap range
        // check, so null / non-heap (incl. colour-only null after flip) aborts as
        // "GetUnitIdxAt OOB addr=0". FixOldTaggedRefField then nulls the slot.
        // nullptr here is dual: non-heap/null gate OR no to-version for a heap from.
        // Soft-resolve paths must not CAS-null on the non-heap reading (RO static).
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            return nullptr;
        }
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
        if (fromRegionInfo == nullptr) {
            return nullptr;
        }
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        return space.GetRegionManager().RouteObject(obj);
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

    RefField<> GetAndTryTagRefField(BaseObject* target) const override
    {
        // Null carries no colour (ZGC zAddress: null is never load-bad).
        if (target == nullptr) {
            return RefField<>(static_cast<BaseObject*>(nullptr));
        }
        // Store-good colour: mark-good | current Remembered (OpenJDK zAddress.cpp:83).
        // Mid-evacuation is not a pointer bit (zAddress.hpp:60-128). A from-address is
        // resolved via the region table before it is painted current. If it is still
        // from, paint a stale remap one-hot so the value stays load-bad (ZGC: remapped
        // means the address is already the current location).
        if (IsFromObject(target) || IsGhostFromObject(target)) {
            BaseObject* to = FindToVersion(target);
            if (to != nullptr) {
                target = to;
            }
        }
        const bool stillFrom = IsFromObject(target) || IsGhostFromObject(target);
        const Uptr notCurrent = REMAP_COLOUR_MASK ^ currentRemapColour;
        const Uptr staleOneHot = notCurrent & -notCurrent;
        const Uptr remap = stillFrom ? staleOneHot : currentRemapColour;
        const Uptr storeColour = remap | currentMarkedYoung | currentMarkedOld | currentRemembered;
        return RefField<>(target, storeColour);
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
            const char* v = std::getenv("MRT_GCV2_PLAIN_ROOTS");
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
    BaseObject* ResolveMinorReference(RefField<>& field) const;
    BaseObject* ResolveMinorReference(RootSlot& root) const;
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
                             MinorInteriorBaseMap* interiorBasesOut = nullptr);
    bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr) const;
    bool FixMinorEvacuatedSlot(RootSlot& root) const;
    bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase) const;
    void FixMinorRootSlots();
    void FixMinorRootSlotsParallel(GCThreadPool* threadPool);
    void FixMinorObjectSlots(BaseObject* object);
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
    void FixOldTaggedRefField(BaseObject* holder, RefField<>& field);
    void PreforwardConcurrencyModelRoots();
    void PostTrace();
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
