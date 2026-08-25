// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <array>
#include <atomic>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <csignal>
#endif
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

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Base/SysCall.h"
#endif
#include "Concurrency/Concurrency.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/NullRouteCaller.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Common/ColourPredicates.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Verify/VerifyRegions.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "securec.h"
#endif
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
static_assert(sizeof(RefField<false>) == 8, "RefField colour layout must preserve the 64-bit ABI");
std::atomic<size_t> g_forwardRaceTotalCount{ 0 };
std::atomic<size_t> g_forwardRaceStillBadCount{ 0 };

void ReportForwardRaceCounts()
{
}

// paramzero: crash-time snapshot of (a) Mode-A stack slot -0x50(%rbp) and
// (b) heap CAS-null arm counters. Gate = MRT_GCV2_NULLSLOT (same as nullslot);
// atomics themselves are always-on. Called from SignalManager::EmitCrashRec.
// AS-safe-ish: only stack reads + write(2); no heap, no lock.
void EmitParamzeroCrashProbe(uintptr_t rbp, uintptr_t rbx, uintptr_t rip)
{
    if (!NullslotProbeEnabled()) {
        return;
    }
    // Read -0x50(%rbp) = entry rsi spill (nullwriter objdump). Best-effort:
    // if the page is unmapped this may re-fault; nested SEGV is accepted.
    unsigned long slot50 = 0;
    unsigned long slot30 = 0;
    unsigned long slot40 = 0;
    unsigned long savedRbp = 0;
    unsigned long retAddr = 0;
    unsigned long callerSlot50 = 0;
    int slotOk = 0;
    if (rbp != 0 && rbp > 0x1000UL) {
        const unsigned long* fp = reinterpret_cast<const unsigned long*>(rbp);
        // Frame layout: [rbp]=saved rbp, [rbp+8]=return, [rbp-0x50]=rsi spill.
        savedRbp = fp[0];
        retAddr = fp[1];
        const unsigned long* slot50p =
            reinterpret_cast<const unsigned long*>(rbp - 0x50UL);
        const unsigned long* slot30p =
            reinterpret_cast<const unsigned long*>(rbp - 0x30UL);
        const unsigned long* slot40p =
            reinterpret_cast<const unsigned long*>(rbp - 0x40UL);
        slot50 = *slot50p;
        slot30 = *slot30p;
        slot40 = *slot40p;
        slotOk = 1;
        if (savedRbp != 0 && savedRbp > 0x1000UL) {
            const unsigned long* cfp =
                reinterpret_cast<const unsigned long*>(savedRbp - 0x50UL);
            callerSlot50 = *cfp;
        }
    }
    // rbx at crash is the reloaded -0x50 value (Mode A: 0).
    // entry_is_zero: if slot50==0 at crash AND product never rewrites that spill
    // before reload (objdump: only one store at 6ef849 before 6ef8ee load), then
    // the argument was already 0 at function entry.
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][nullslot] path=paramzero_frame n=0 "
                      "rbp=%#lx rip=%#lx rbx=%#lx "
                      "slot_m50=%#lx slot_m30=%#lx slot_m40=%#lx "
                      "saved_rbp=%#lx ret=%#lx caller_slot_m50=%#lx slot_ok=%d "
                      "entry_arg_zero=%d\n",
                      static_cast<unsigned long>(rbp), static_cast<unsigned long>(rip),
                      static_cast<unsigned long>(rbx), slot50, slot30, slot40, savedRbp,
                      retAddr, callerSlot50, slotOk, (slotOk && slot50 == 0) ? 1 : 0);
    if (n > 0) {
        (void)write(STDERR_FILENO, line, static_cast<size_t>(n));
    }
    n = sprintf_s(line, sizeof(line),
                  "[GCV2][nullslot] path=paramzero_cas n=0 "
                  "fix_resolve_cas=%zu f3_fix_oldtag=%zu remset_stale=%zu "
                  "resolve_root_entry=%zu resolve_root_old=%zu resolve_root_healNull=%zu "
                  "fix_minor_roots_calls=%zu\n",
                  g_nullslotResolve.load(std::memory_order_relaxed),
                  g_nullslotF3.load(std::memory_order_relaxed),
                  g_nullslotRemset.load(std::memory_order_relaxed),
                  g_resolveRootEntry.load(std::memory_order_relaxed),
                  g_resolveRootOld.load(std::memory_order_relaxed),
                  g_resolveRootHealNull.load(std::memory_order_relaxed),
                  g_fixMinorRootSlotsCalls.load(std::memory_order_relaxed));
    if (n > 0) {
        (void)write(STDERR_FILENO, line, static_cast<size_t>(n));
    }
}
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
namespace {
struct UntagRefFieldBreadcrumb {
    const void* holder = nullptr;
    const void* field = nullptr;
    const void* target = nullptr;
    const void* caller = nullptr;
    size_t fieldOffset = 0;
    volatile sig_atomic_t active = 0;
};

thread_local UntagRefFieldBreadcrumb untagRefFieldBreadcrumb;
} // namespace

void PrintUntagRefFieldBreadcrumb() noexcept
{
    if (untagRefFieldBreadcrumb.active == 0) {
        return;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    char buf[320];
    int n = sprintf_s(buf, sizeof(buf),
                      "%d E GC untag breadcrumb: holder=%p field=%p field_offset=%zu target=%p caller_pc=%p\n",
                      static_cast<int>(GetTid()), untagRefFieldBreadcrumb.holder, untagRefFieldBreadcrumb.field,
                      untagRefFieldBreadcrumb.fieldOffset, untagRefFieldBreadcrumb.target,
                      untagRefFieldBreadcrumb.caller);
    if (n > 0) {
        (void)write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}
#endif



extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(Heap::GetBarrier().ReadReference(this,
            HeapSlotAt<false>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(Heap::GetBarrier().ReadReference(this,
            HeapSlotAt<false>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler WCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__)
    size_t i = 0;
    if (!cycleWorkStackMtx.try_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    for (auto it = cycleRefWorkStack.begin(); it != cycleRefWorkStack.end(); i++) {
        ScopedObjectAccess soa;
        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleWorkStackMtx.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }
        BaseObject* exportObj = it->first;
        auto& heap = Heap::GetHeap();
        auto id = static_cast<ExportObject*>(exportObj)->GetId();
        if (!heap.CheckExportObjState(id, exportObj)) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        if (resurrectedExportObjectes.find(exportObj) != resurrectedExportObjectes.end() ||
            resurrectedExportObjectesForwardPhase.find(exportObj) != resurrectedExportObjectesForwardPhase.end()) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        auto externObjs = it->second;
        void* returnUnit = nullptr;
        for (auto externObj : externObjs) {
            auto resolveHook = GetCrossRefHandler(externObj);
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
        }
        heap.SetExportObjActiveState(id, false);
        it++;
    }
    cycleWorkStackMtx.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void WCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}
void WCollector::DoGarbageCollection()
{
    // Free the forwarding entry tables retired during the previous cycle. ZGC recycles its
    // forwarding arena at the next cycle's ZRelocationSetInstallTask (zRelocationSet.cpp:91-96),
    // one whole phase after ZHeap::free_page released the page the forwarding described; that gap
    // is why ZForwarding::find may run holding no reference (zRelocate.cpp:382-393). Reclaiming at
    // the head of a cycle gives ours the same gap: by now every reader that could have loaded one
    // of these pointers has been through a phase transition.
    ForwardingTable::ReclaimRetired("cycle-start");
    // ZGC: not-selected pages are ordinary candidates next cycle
    // (zRelocationSetSelector.cpp:114-196). Expire last cycle's Exempt-kept
    // before Assemble / PrepareYoung so they re-enter the selector.
    reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ExpireKeptFromPreviousCycle();
    if (gcReason == GC_REASON_YOUNG) {
        DoYoungGarbageCollection();
        Collector::ReportMarkGoodHeapGateCounts();
        return;
    }
    // oracleblack: a major is young + old, as in ZGC (zGeneration.cpp ZGenerationOld
    // collections run the young generation first; a standalone old cycle does not exist).
    // Ours ran old-only: under a config whose young trigger never fires (cjpm at 12GB,
    // youngRegionTriggerBytes=32MB unreached), young regions were never marked by anyone,
    // yet three separate paths (ForwardRegion knownEmpty, its unmarked arm, and
    // Assemble->from-space route=5 collect) judged them by the OLD view and freed live
    // young objects wholesale (f3-livehole census; TraceClear kind=coll_live).
    // Running the young cycle first gives every young region a real examination each major.
    //
    // The nested young cycle must run under its own identity: CopyCollector::ForwardFromSpace
    // (CopyCollector.cpp:189-193) and every other generation dispatch key on gcReason, so a
    // young evacuation executed while gcReason==HEU instantiates the Old templates and trips
    // GetRouteMarkView's generation CHECK on every young-enrolled region (RegionInfo.h:385).
    {
        const GCReason majorReason = gcReason;
        GCStats& stats = GetGCStats();
        const GCReason majorStatsReason = stats.reason;
        gcReason = GC_REASON_YOUNG;
        // MarkAndRememberNewValue dispatches its young paint vs major SATB leg
        // through GCStats::reason. Publish the same nested-cycle identity there.
        stats.reason = GC_REASON_YOUNG;
        DoYoungGarbageCollection();
        gcReason = majorReason;
        stats.reason = majorStatsReason;
    }
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();
    reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().FinishIncompleteFromRegions();

    // Preserve young remembered-set faces across old/full collection. ZGC old
    // relocation transfers remembered fields; it does not globally erase the
    // young current face. ClearRegion/TransferObjectSlots remain the authorities
    // for reclaimed or moved holders (zRelocate.cpp:652-731).
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);
    // FlipTagID just turned this cycle's current-tags into IsOldPointer. F3 pre-flip only
    // saw the previous tag generation. This pass must NOT filter IsSurvivedObject:
    // after Forward, live holders are in to-space without mark bits at the new addr.
    //
    // This walk exists because a reference could not say for itself that its colour was stale, so
    // someone had to strip the old tag off every one of them before the tag was reused. Once the
    // read barrier heals a stale colour on the way past (FixOldTaggedRefField), the walk has
    // nothing left to do -- but that claim needs measuring before the walk goes away for good, so
    // it is a switch rather than a deletion. Nobody has measured what this pass costs.
    InvalidateOldTaggedRefs(false);
    reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ExpireKeptFromPreviousCycle();
    if (HealCoverage::kHealCoverageCensus) {
        HealCoverage::CensusAfterPublication(
            currentRemapColour, FlipSeq().load(std::memory_order_relaxed), "major-postflip");
    }

    CollectSmallSpace();
    // domainon: major path coverage dump (Record may fire under non-YOUNG if youngRegion).
    PromotedRegionDomain::DumpCoverageByReason("post-major");
    // retmid: do NOT StampCensusBoundaries / PromoteAllRegions here.
    // Ablation D (both major STWs disabled) restores mid_alloc 5/5; any of
    // Flush/Stamp/Promote in these STWs reintroduces 0/5 or residual 甲 under
    // FYS=0 SKIP_PINNED=1 512MB. Retained-liveness still applies on residual and
    // in-place promote paths that already Preserve + RecordPromotedCrossGenEdges.
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
    Collector::ReportMarkGoodHeapGateCounts();

}
bool WCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
namespace MapleRuntime {
namespace ZgcInvariants {
// flipseq bridge: keeps ZgcInvariants.cpp from having to include the collector header.
uint64_t WCollectorFlipSeqForProbe() { return WCollector::FlipSeq().load(std::memory_order_relaxed); }
BaseObject* ProbeFindToVersion(BaseObject* obj)
{
    Collector& c = Heap::GetHeap().GetCollector();
    return c.FindToVersion(obj);
}
} // namespace ZgcInvariants
} // namespace MapleRuntime
