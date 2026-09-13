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
#include "Heap/Verify/Stw2CurrentAudit.h"
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
#if defined(MRT_GC_UNIT_TESTS)
    if (cycleRefHandlerForTest != nullptr) {
        return cycleRefHandlerForTest;
    }
#endif
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__) || defined(MRT_GC_UNIT_TESTS)
    // Leave saferegion before acquiring either owner. The resolver owner is not
    // used by GC; it preserves the former single-resolver property while the
    // root-carrier owner is released around every managed callback.
    ScopedObjectAccess soa;
    std::unique_lock<std::mutex> resolverLock(cycleResolverMtx, std::try_to_lock);
    if (!resolverLock.owns_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    size_t i = 0;
    std::unique_lock<std::mutex> cycleLock(cycleWorkStackMtx, std::try_to_lock);
    if (!cycleLock.owns_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    std::unordered_set<U32> resolvedIds;
    for (;;) {
        auto it = cycleRefWorkStack.begin();
        while (it != cycleRefWorkStack.end()) {
            BaseObject* candidate = it->first;
            U32 candidateId = static_cast<ExportObject*>(candidate)->GetId();
            if (resolvedIds.find(candidateId) != resolvedIds.end()) {
                ++it;
                continue;
            }
            auto& heap = Heap::GetHeap();
            if (!heap.CheckExportObjState(candidateId, candidate) ||
                resurrectedExportObjectes.find(candidate) != resurrectedExportObjectes.end() ||
                resurrectedExportObjectesForwardPhase.find(candidate) !=
                    resurrectedExportObjectesForwardPhase.end()) {
                cycleRefProgress.erase(candidateId);
                it = cycleRefWorkStack.erase(it);
                continue;
            }
            break;
        }
        if (it == cycleRefWorkStack.end()) {
            break;
        }

        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleLock.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }

        U32 id = static_cast<ExportObject*>(it->first)->GetId();
        size_t externIndex = cycleRefProgress[id];
        void* returnUnit = nullptr;
        for (;;) {
            // A GC preforward pass may replace the map key and list elements
            // while the callback is parked. Re-find by stable export id and
            // fetch the current addresses before each managed invocation.
            it = std::find_if(cycleRefWorkStack.begin(), cycleRefWorkStack.end(),
                [id](const auto& entry) {
                    return static_cast<ExportObject*>(entry.first)->GetId() == id;
                });
            if (it == cycleRefWorkStack.end() || externIndex >= it->second.size()) {
                break;
            }
            if (GetGCPhase() == GC_PHASE_PREFORWARD) {
                cycleLock.unlock();
                CJ_MRT_RolveCycleRef();
                return;
            }
            BaseObject* exportObj = it->first;
            auto externIt = it->second.begin();
            std::advance(externIt, static_cast<ptrdiff_t>(externIndex));
            BaseObject* externObj = *externIt;
            auto resolveHook = GetCrossRefHandler(externObj);

            // ResolveCycleRefStub enters managed code. A late safepoint can
            // park there, so the GC-owned root carrier must be available while
            // the callback runs. cycleResolverMtx alone prevents duplicate
            // resolver delivery and is never acquired by a GC consumer.
            cycleLock.unlock();
#if defined(MRT_GC_UNIT_TESTS)
            // The minimal gc_unit process has no scheduler-owned CJThread for
            // the assembly N2C adapter. The injected handler still runs from
            // this product call site and enters the real HandleSafepoint path;
            // production always takes the adapter below.
            if (cycleRefHandlerForTest != nullptr) {
                resolveHook(exportObj, externObj);
            } else {
                ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
            }
#else
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
#endif
            cycleLock.lock();

            // The callback was delivered while the full entry stayed in the
            // GC-visible carrier. Commit that delivery before observing a
            // phase change so a PREFORWARD repost resumes at the next item
            // instead of delivering this one again.
            ++externIndex;
            cycleRefProgress[id] = externIndex;
        }

        auto& heap = Heap::GetHeap();
        heap.SetExportObjActiveState(id, false);
        cycleRefProgress.erase(id);
        resolvedIds.insert(id);
        ++i;
    }
    cycleLock.unlock();
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
    if (GetCycleReason() == GC_REASON_YOUNG) {
        DoYoungGarbageCollection();
        Collector::ReportMarkGoodHeapGateCounts();
        return;
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
    MergeResurrectExportObjects(Generation::Old);
    PostResolveCycleTask();
    FlipTagID();
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
    // This diagnostic observer does not consume the object. Keep Unavailable
    // observable without taking the product consumers' fail-closed exit.
    const FindToVersionResult observed = c.FindToVersion(obj, c.ActiveForwardingGeneration());
    return observed.found();
}
} // namespace ZgcInvariants
} // namespace MapleRuntime
