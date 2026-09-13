// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkEngine.h"
#include "Heap/Collector/MarkStripe.h"
#include "TracingCollector.h"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/M0ExitDiagnostics.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/VerifyMarkingStacks.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
std::function<void(GCWorkers::Generation, TracingCollector::RootSet&)> TracingCollector::testRootsResult;
std::function<void()> TracingCollector::testCyclePrepared;
std::function<void()> TracingCollector::testYoungMarkStarted;
#endif

// ZMark::_ncontinue (zMark.cpp:975-981). Always on so a zero is readable as
// "the pre-pause test was right every time" rather than "nobody is counting".
std::atomic<size_t> g_markTerminateContinue{ 0 };
std::atomic<size_t> g_markTerminatePauses{ 0 };
std::atomic<size_t> g_markTerminateFlushed{ 0 };
std::atomic<bool> g_markTerminateAtexitInstalled{ false };
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<uint64_t> g_markTerminateMaxPauseNs{ 0 };
std::atomic<size_t> g_markTerminatePauseAllocBlack{ 0 };
std::atomic<size_t> g_markTerminatePauseY2y{ 0 };
std::atomic<size_t> g_markTerminateClosureDuringPause{ 0 };
std::atomic<size_t> g_weakDiscoveryCount{ 0 };
#endif

void NoteMarkTerminatePause() { g_markTerminatePauses.fetch_add(1, std::memory_order_relaxed); }

void NoteMarkTerminateFlushed(size_t n)
{
    g_markTerminateFlushed.fetch_add(n, std::memory_order_relaxed);
}

void NoteMarkTerminateContinue(size_t stackSize)
{
    const size_t nContinue = g_markTerminateContinue.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG(RTLOG_ERROR, "[GCV2][markterm] pause found unflushed SATB work: ncontinue=%zu stack=%zu", nContinue,
        stackSize);
}

void ReportMarkTerminateContinue()
{
    if (!g_markTerminateAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
        (void)std::atexit([]() {
            LOG(RTLOG_ERROR, "[GCV2][markterm] atexit pauses=%zu flushedInPause=%zu ncontinue=%zu",
                g_markTerminatePauses.load(std::memory_order_relaxed),
                g_markTerminateFlushed.load(std::memory_order_relaxed),
                g_markTerminateContinue.load(std::memory_order_relaxed));
        });
    }
}

#if defined(MRT_TESTABLE_INTERNALS)
void ResetMarkTerminateTestReceipt()
{
    g_markTerminateContinue.store(0, std::memory_order_relaxed);
    g_markTerminatePauses.store(0, std::memory_order_relaxed);
    g_markTerminateFlushed.store(0, std::memory_order_relaxed);
    g_markTerminateMaxPauseNs.store(0, std::memory_order_relaxed);
    g_markTerminatePauseAllocBlack.store(0, std::memory_order_relaxed);
    g_markTerminatePauseY2y.store(0, std::memory_order_relaxed);
    g_markTerminateClosureDuringPause.store(0, std::memory_order_relaxed);
}

MarkTerminateTestReceipt ReadMarkTerminateTestReceipt()
{
    return { g_markTerminatePauses.load(std::memory_order_relaxed),
             g_markTerminateFlushed.load(std::memory_order_relaxed),
             g_markTerminateContinue.load(std::memory_order_relaxed),
             g_markTerminateMaxPauseNs.load(std::memory_order_relaxed),
             g_markTerminatePauseAllocBlack.load(std::memory_order_relaxed),
             g_markTerminatePauseY2y.load(std::memory_order_relaxed),
             g_markTerminateClosureDuringPause.load(std::memory_order_relaxed) };
}

void NoteMarkTerminatePauseDuration(uint64_t pauseNs)
{
    uint64_t observed = g_markTerminateMaxPauseNs.load(std::memory_order_relaxed);
    while (observed < pauseNs &&
           !g_markTerminateMaxPauseNs.compare_exchange_weak(observed, pauseNs, std::memory_order_relaxed)) {}
}

void NoteMarkTerminatePauseProducers(size_t allocBlack, size_t y2y)
{
    g_markTerminatePauseAllocBlack.fetch_add(allocBlack, std::memory_order_relaxed);
    g_markTerminatePauseY2y.fetch_add(y2y, std::memory_order_relaxed);
}

void NoteTraceYoungClosureDuringPause()
{
    g_markTerminateClosureDuringPause.fetch_add(1, std::memory_order_relaxed);
}

void ResetWeakDiscoveryTestReceipt()
{
    g_weakDiscoveryCount.store(0, std::memory_order_relaxed);
}

WeakDiscoveryTestReceipt ReadWeakDiscoveryTestReceipt()
{
    return { g_weakDiscoveryCount.load(std::memory_order_relaxed) };
}
#endif

namespace {
struct SkippedStackMapCounts {
    std::atomic<size_t> zeroEntries{ 0 };
    std::atomic<size_t> pcMiss{ 0 };
    std::atomic<size_t> zeroRootIndices{ 0 };
};

SkippedStackMapCounts g_skippedStackMapCounts;
thread_local size_t g_currentThreadRootMapMissCount = 0;

const char* StackMapInvalidReasonName(StackMapInvalidReason reason)
{
    switch (reason) {
        case StackMapInvalidReason::NONE:
            return "none";
        case StackMapInvalidReason::ZERO_ENTRIES:
            return "present-but-zero-entries";
        case StackMapInvalidReason::PC_MISS:
            return "pc-miss-exact";
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            return "zero-root-indices";
    }
    return "unknown";
}

void ResetSkippedStackMapCounts()
{
    g_skippedStackMapCounts.zeroEntries.store(0, std::memory_order_relaxed);
    g_skippedStackMapCounts.pcMiss.store(0, std::memory_order_relaxed);
    g_skippedStackMapCounts.zeroRootIndices.store(0, std::memory_order_relaxed);
}

void RecordRootMapMiss(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP, uintptr_t frameIP,
                       const Mutator& mutator)
{
    (void)reason;
    (void)frame;
    (void)startIP;
    (void)frameIP;
    (void)mutator;
    ++g_currentThreadRootMapMissCount;
}

// Per-process sample cap for SKIPPED_WHO lines (HotSpot-style named frames).
// Default 16 distinct (reason,symbol) pairs; override with MRT_GCV2_SKIPPED_WHO_MAX.
std::atomic<size_t> g_skippedWhoPrinted{ 0 };

ATTR_NO_INLINE void RecordSkippedStackMap(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP,
                                          uintptr_t frameIP)
{
    std::atomic<size_t>* skippedCount = &g_skippedStackMapCounts.zeroRootIndices;
    switch (reason) {
        case StackMapInvalidReason::ZERO_ENTRIES:
            skippedCount = &g_skippedStackMapCounts.zeroEntries;
            break;
        case StackMapInvalidReason::PC_MISS:
            skippedCount = &g_skippedStackMapCounts.pcMiss;
            break;
        case StackMapInvalidReason::NONE:
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            skippedCount = &g_skippedStackMapCounts.zeroRootIndices;
            break;
    }
    skippedCount->fetch_add(1, std::memory_order_relaxed);

    size_t printed = g_skippedWhoPrinted.load(std::memory_order_relaxed);
    if (printed < 16) {
        if (g_skippedWhoPrinted.compare_exchange_strong(printed, printed + 1, std::memory_order_relaxed)) {
            CString symbol = frame.GetFuncName();
            uintptr_t fa = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
            U32 pcOff = (frameIP >= startIP) ? static_cast<U32>(frameIP - startIP) : 0;
            LOG(RTLOG_ERROR,
                "GC stack map SKIPPED_WHO reason=%s symbol=%s start_ip=%p frame_ip=%p pc_off=%u fa=%p "
                "frameType=%u (PC_MISS=exact offset not in stackmap; ZERO_ENTRIES=RECORD_NUM=0)",
                StackMapInvalidReasonName(reason), symbol.IsEmpty() ? "?" : symbol.Str(),
                reinterpret_cast<void*>(startIP), reinterpret_cast<void*>(frameIP), pcOff,
                reinterpret_cast<void*>(fa), static_cast<unsigned>(frame.GetFrameType()));
        }
    }
}

void ReportSkippedStackMapCounts()
{
    size_t zeroEntries = g_skippedStackMapCounts.zeroEntries.load(std::memory_order_relaxed);
    size_t pcMiss = g_skippedStackMapCounts.pcMiss.load(std::memory_order_relaxed);
    size_t zeroRootIndices = g_skippedStackMapCounts.zeroRootIndices.load(std::memory_order_relaxed);
    if (zeroEntries != 0 || pcMiss != 0 || zeroRootIndices != 0) {
        LOG(RTLOG_ERROR,
            "GC stack map warning: SKIPPED_ZERO_ENTRIES=%zu SKIPPED_PC_MISS=%zu "
            "SKIPPED_OTHER_ZERO_ROOT_INDICES=%zu",
            zeroEntries, pcMiss, zeroRootIndices);
    }
}
} // namespace

size_t TracingCollector::CurrentThreadRootMapMissCount()
{
    return g_currentThreadRootMapMissCount;
}


// Fill gc roots entry to buckets
void StaticRootTable::RegisterRoots(StaticRootArray* addr, U32 size)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    // L741: map::insert keeps first value; must not inflate totalRootsCount on dup key.
    auto result = gcRootsBuckets.insert(std::pair<StaticRootArray*, U32>(addr, size));
    if (!result.second) {
        LOG(RTLOG_ERROR,
            "StaticRootTable::RegisterRoots duplicate key %p size %u (kept size %u); totalRootsCount not increased",
            addr, size, result.first->second);
        return;
    }
    totalRootsCount += size;
}

void StaticRootTable::UnregisterRoots(StaticRootArray* addr, U32 size)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    auto iter = gcRootsBuckets.find(addr);
    if (iter == gcRootsBuckets.end()) {
        LOG(RTLOG_ERROR, "StaticRootTable::UnregisterRoots missing key %p size %u", addr, size);
        return;
    }
    if (iter->second != size) {
        LOG(RTLOG_ERROR,
            "StaticRootTable::UnregisterRoots size mismatch key %p caller %u registered %u; using registered",
            addr, size, iter->second);
        totalRootsCount -= iter->second;
    } else {
        totalRootsCount -= size;
    }
    gcRootsBuckets.erase(iter);
}

#ifdef MRT_TESTABLE_INTERNALS
USize StaticRootTable::RootCountForTesting()
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    return totalRootsCount;
}
#endif

void StaticRootTable::VisitRoots(const RootSlotVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    U32 gcRootsSize = 0;
    std::unordered_set<RootSlot*> visitedSet;
    for (auto iter = gcRootsBuckets.begin(); iter != gcRootsBuckets.end(); iter++) {
        gcRootsSize = iter->second;
        StaticRootArray* array = iter->first;
        for (USize i = 0; i < gcRootsSize; i++) {
            RootSlot* root = array->content[i];
            // make sure to visit each static root only once time.
            if (!visitedSet.insert(root).second) {
                continue;
            }
            visitor(*root);
        }
    }
}

void ExportRootTable::VisitGCRoots(const RootVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(tableMutex);
    for (auto &rootInfo : exportRoots) {
        visitor(rootInfo.exportObj);
    }
}
struct MajorMarkShared {
    TracingCollector* collector = nullptr;
    size_t workerCount = 0;
    MarkDomain* domain = nullptr;
    bool partial = false;
    std::atomic<size_t> newlyMarked{ 0 };

    MarkStripeSet& Stripes() { return domain->Stripes(); }
    MarkingSMR& Smr() { return domain->Smr(); }
    MarkTerminate& Terminate() { return domain->Terminate(); }
    MarkThreadLocalStacks& Stacks() { return domain->Stacks(); }

    size_t StripeFor(const MarkStackEntry& entry) const
    {
        MAddress address = 0;
        if (entry.partialArray()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(entry.object());
        }
        return domain->Stripes().StripeForAddress(address);
    }
};

class ConcurrentMarkingWork : public GCRestartableWorkerTask {
public:
    explicit ConcurrentMarkingWork(MajorMarkShared& shared) : shared(shared) {}

    void ResizeWorkers(uint32_t workers) override
    {
        shared.workerCount = workers;
        shared.domain->ResizeWorkers(workers);
    }

    void Work(uint32_t workerId) override
    {
        MarkContext local(shared.workerCount, workerId, shared.Stripes(), shared.Stacks());
        size_t nNewlyMarked = 0;
        TracingCollector::WorkStack staging;
        (void)MarkEngine::FollowWork(local, shared.Smr(), shared.Stripes(), shared.Terminate(), workerId,
                                     shared.partial,
                                     [this, &nNewlyMarked, &staging, &local](const MarkStackEntry& entry) {
                                         ProcessEntry(local, entry, nNewlyMarked, staging);
                                     },
                                     nullptr, nullptr, shared.domain);
        (void)local.Stacks().Flush(shared.Stripes(), true);
        local.Cache().Flush();
        shared.newlyMarked.fetch_add(nNewlyMarked, std::memory_order_relaxed);
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::TASK_EXIT,
                                         VerifyMarkingStacks::MarkingContainer::LOCAL,
                                         local.Stacks().Population(), 0, workerId);
    }

private:
    void PublishStaging(MarkContext& ctx, TracingCollector::WorkStack& staging)
    {
        while (!staging.empty()) {
            const MarkStackEntry next = staging.back();
            staging.pop_back();
            const size_t stripeIndex = shared.StripeFor(next);
            const bool publish = stripeIndex != ctx.StripeId();
            ctx.Stacks().Push(shared.Stripes(), stripeIndex, next, publish);
        }
    }

    void ProcessEntry(MarkContext& ctx, const MarkStackEntry& entry, size_t& nNewlyMarked,
                      TracingCollector::WorkStack& staging)
    {
        TracingCollector& collector = *shared.collector;
        if (UNLIKELY(MarkPartialArray::IsPartialArrayEntry(entry))) {
            collector.FollowPartialArray(entry, staging);
            PublishStaging(ctx, staging);
            return;
        }
        BaseObject* obj = entry.object();
        if (!Collector::PlausibleManagedObjectGate("ConcurrentMarkingWork.pop", obj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(obj);
            if (host == nullptr || host == obj ||
                !Collector::PlausibleManagedObjectGate("ConcurrentMarkingWork.host", host)) {
                return;
            }
            obj = host;
        }
        const bool wasMarked = collector.MarkEntryObject(obj, entry, &ctx.Cache());
        if ((!entry.mark() || !wasMarked) && entry.follow()) {
            if (entry.mark()) {
                nNewlyMarked++;
            }
            if (!obj->HasRefField()) {
                return;
            }
            SurvNodeDiag::NoteFollowHolder(obj, SurvNodeDiag::FOLLOW_SCAN);
            if (UNLIKELY(obj->IsWeakRef())) {
                collector.DiscoverWeakReference(obj, staging);
            } else {
                collector.TraceObjectRefFields(obj, staging, entry.finalizable());
            }
            PublishStaging(ctx, staging);
        } else if (entry.mark() && wasMarked) {
            SurvNodeDiag::NoteFollowHolder(obj, SurvNodeDiag::FOLLOW_SKIP_MARKED);
        }
    }

    MajorMarkShared& shared;
};

// ZMarkRootsTask (zMark.cpp): generation workers claim independent root work.
class ExportRootsTracingWork : public GCWorkerTask {
public:
    ExportRootsTracingWork(TracingCollector& tc, TracingCollector::WorkStack&& stack)
        : collector(tc)
    {
        while (!stack.empty()) {
            roots.push_back(stack.back());
            stack.pop_back();
        }
    }

    void Work(uint32_t workerId) override
    {
        TracingCollector::WorkStack workStack;
        MarkLiveCache liveCache(1);
        for (;;) {
            if (workStack.empty()) {
                const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
                if (index >= roots.size()) {
                    break;
                }
                workStack.push_back(roots[index]);
            }
            const MarkStackEntry entry = workStack.back();
            workStack.pop_back();
            if (entry.partialArray()) {
                collector.FollowPartialArray(entry, workStack);
                continue;
            }
            BaseObject* obj = entry.object();
            const bool wasMarked = collector.MarkEntryObject(obj, entry, &liveCache);
            if (!wasMarked && entry.follow()) {
                collector.DFSTraceExportObject(obj, entry.finalizable());
            }
        }
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::TASK_EXIT,
                                         VerifyMarkingStacks::MarkingContainer::TASK, workStack.size(), 0,
                                         workerId);
    }
private:
    TracingCollector& collector;
    std::vector<MarkStackEntry> roots;
    std::atomic<size_t> cursor { 0 };
};
void TracingCollector::VisitStackRoots(const RootVisitor& visitor, RegSlotsMap& regSlotsMap, const FrameInfo& frame,
                                       Mutator& mutator)
{
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress);
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    DLOG(ENUM, "visit frame 0x%zx-@0x%zx, fp 0x%zx", startIP, frameIP, frameAddress);
    auto gcInfo = GCInfoNode::BuildNodeForTrace(startIP, frameIP, frame.mFrame.GetFA());
    auto slotDebugFunc = [&gcInfo](SlotBias off, BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            gcInfo.InsertSlotRoots<true>(off, root);
        } else {
            gcInfo.InsertSlotRoots<false>(off, root);
        }
    };
    auto regDebugFunc = [&gcInfo](RegisterNum i, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            gcInfo.InsertRegRoot<true>(i, root);
        } else {
            gcInfo.InsertRegRoot<false>(i, root);
        }
    };
#else
    SlotDebugVisitor slotDebugFunc = nullptr;
    RegDebugVisitor regDebugFunc = nullptr;
#endif
    // gcvroot: optional rich root diagnostics (MRT_GCV2_VERIFY_ROOTS=1). Does not replace CHECKs.
    if (VerifyRoots::Enabled()) {
        RootVerifyContext vctx;
        vctx.phase = "VisitStackRoots";
        vctx.kind = RootKind::SLOT_STACK;
        vctx.startIP = startIP;
        vctx.frameIP = frameIP;
        vctx.frameFA = frameAddress;
        vctx.ownerMutator = &mutator;
        static thread_local char gcvrootNameBuf[256];
        gcvrootNameBuf[0] = '\0';
        CString fname = frame.GetFuncName();
        if (fname.Str() != nullptr) {
            std::strncpy(gcvrootNameBuf, fname.Str(), sizeof(gcvrootNameBuf) - 1);
            gcvrootNameBuf[sizeof(gcvrootNameBuf) - 1] = '\0';
            vctx.funcName = gcvrootNameBuf;
        }
        SlotDebugVisitor verifySlot = VerifyRoots::MakeSlotDebugVisitor(vctx);
        RegDebugVisitor verifyReg = VerifyRoots::MakeRegDebugVisitor(vctx);
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
        auto prevSlot = slotDebugFunc;
        auto prevReg = regDebugFunc;
        slotDebugFunc = [prevSlot, verifySlot](SlotBias off, BaseObject* root) {
            verifySlot(off, root);
            if (prevSlot) {
                prevSlot(off, root);
            }
        };
        regDebugFunc = [prevReg, verifyReg](RegisterNum i, const BaseObject* root) {
            verifyReg(i, root);
            if (prevReg) {
                prevReg(i, root);
            }
        };
#else
        slotDebugFunc = verifySlot;
        regDebugFunc = verifyReg;
#endif
    }
    // introot: use HeapReferenceMap so base/derived pairs are available. RootMap only
    // carries reg/slot roots and silently drops derived (RawArray+8 held across safepoint).
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(false);
    M0ExitDiagnostics::StackMapScope m0StackMap(
        heapMap.IsValid(), builder.GetInvalidReason(), startIP, frameIP, frameAddress);
    RootVisitor slotVisitor = visitor;
    RootVisitor regVisitor = visitor;

    if (heapMap.IsValid()) {
        heapMap.VisitSlotRoots(slotVisitor, slotDebugFunc);
        if (!heapMap.VisitRegRoots(regVisitor, regDebugFunc, regSlotsMap)) {
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
            mutator.PushFrameInfoForTrace(gcInfo);
#endif
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
        // Mark the base of each derived pair. Derived slot itself is not an object root;
        // leave it for PreForward's derived visitor to rewrite after evacuation.
        DerivedPtrVisitor derivedMark = [&visitor](BasePtrType basePtr, DerivedSlot& derivedPtr) {
            (void)derivedPtr;
            if (is_null(basePtr)) {
                return;
            }
            // Peel colour if present so gate/PushRoot see the real address.
            // The stack-map base remains committed until this root pass completes.
            BaseObject* base = to_object(safe(uncolor_bits(to_zpointer(raw(basePtr)))));
            if (base == nullptr) {
                return;
            }
            ObjectRef baseRef;
            StorePlain(baseRef, from_object(base));
            visitor(baseRef);
        };
        heapMap.VisitDerivedPtr(derivedMark, nullptr, regSlotsMap);
    } else {
        RecordRootMapMiss(builder.GetInvalidReason(), frame, startIP, frameIP, mutator);
    }
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    mutator.PushFrameInfoForTrace(gcInfo);
#endif
    heapMap.RecordCalleeSaved(regSlotsMap);

}

void TracingCollector::DiscoverWeakReference(BaseObject* reference, WorkStack& workStack)
{
    HeapSlot<>& referentField =
        HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
    BaseObject* referent = GetAndTryTagObj(RefSlotKind::WEAK_REFERENT, reference, referentField);
    if (referent == nullptr) {
        return;
    }
    DLOG(TRACE, "trace weakref obj %p ref@%p: 0x%zx", reference, &referent, referent);
    CHECK(DiscoverReference(reference, ReferenceType::WEAK) == ReferenceStatus::DISCOVERED);
#if defined(MRT_TESTABLE_INTERNALS)
    g_weakDiscoveryCount.fetch_add(1, std::memory_order_relaxed);
#endif
    // Deliberately no push/TraceObjectRefFields(referent): discovery must not
    // publish the weak referent into the strong marking work stack.
    (void)workStack;
}

void TracingCollector::VisitHeapReferencesOnStack(const RootVisitor& rootVisitor,
                                                  const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                                  const FrameInfo& frame, Mutator& mutator, bool young)
{
    VisitHeapReferencesOnStack(rootVisitor, rootVisitor, derivedPtrVisitor, regSlotsMap, frame, mutator, young);
}

void TracingCollector::VisitHeapReferencesOnStack(const RootVisitor& regRootVisitor,
                                                  const RootVisitor& slotRootVisitor,
                                                  const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                                  const FrameInfo& frame, Mutator& mutator, bool young)
{
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress);
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(false);
    M0ExitDiagnostics::StackMapScope m0StackMap(
        heapMap.IsValid(), builder.GetInvalidReason(), startIP, frameIP, frameAddress);
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    auto infoNode = GCInfoNodeForFix::BuildNodeForFix(startIP, frameIP, frame.mFrame.GetFA());
    auto slotDebugFunc = [&infoNode](SlotBias off, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            infoNode.InsertSlotRoots<true>(off, root);
        } else {
            infoNode.InsertSlotRoots<false>(off, root);
        }
    };
    auto regDebugFunc = [&infoNode](RegisterNum i, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            infoNode.InsertRegRoot<true>(i, root);
        } else {
            infoNode.InsertRegRoot<false>(i, root);
        }
    };
    auto derivedPtrDebugFunc = [&infoNode](BasePtrType basePtr, DerivedPtrType derivedPtr) {
        infoNode.InsertDerivedPtrRef(basePtr, derivedPtr);
    };
#else
    RegDebugVisitor regDebugFunc = nullptr;
    SlotDebugVisitor slotDebugFunc = nullptr;
    DerivedPtrDebugVisitor derivedPtrDebugFunc = nullptr;
#endif
    DLOG(ENUM, "visit heap-ref 0x%zx-@0x%zx, fp 0x%zx", startIP, frameIP, frameAddress);
    if (heapMap.IsValid()) {
        if (!heapMap.VisitRegRoots(regRootVisitor, regDebugFunc, regSlotsMap, young)) {
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
            mutator.PushFrameInfoForFix(infoNode);
#endif
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
        heapMap.VisitSlotRoots(slotRootVisitor, slotDebugFunc, young);
        // VisitDerivedPtr must be invoked after VisitRegRoots and VisitSlotRoots;
        heapMap.VisitDerivedPtr(derivedPtrVisitor, derivedPtrDebugFunc, regSlotsMap);
    } else {
        RecordSkippedStackMap(builder.GetInvalidReason(), frame, startIP, frameIP);
    }
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    mutator.PushFrameInfoForFix(infoNode);
#endif
    heapMap.RecordCalleeSaved(regSlotsMap);
}

void TracingCollector::RecordStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubCalleeSaved(regSlotsMap, fp);
}

#ifdef __arm__
void TracingCollector::RecordC2NStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordC2NStubCalleeSaved(regSlotsMap, fp);
}

void TracingCollector::RecordExclusiveStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordExclusiveStubCalleeSaved(regSlotsMap, fp);
}
#endif

void TracingCollector::RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubAllRegister(regSlotsMap, fp);
}

void TracingCollector::EnumConcurrencyModelRoots(RootSet& rootSet) const
{
    RootVisitor visitor = [&rootSet, this](ObjectRef& root) {
        if (VerifyRoots::Enabled()) {
            RootVerifyContext ctx;
            ctx.phase = "EnumConcurrencyModelRoots";
            ctx.kind = RootKind::RUNTIME_ROOT;
            ctx.rawValue = raw(root.LoadPlain());
            ctx.hasRawValue = true;
            VerifyRoots::VerifyRootPayload(ctx, &root, nullptr);
        }
        EnumAndTagRawRoot(root, rootSet, Generation::Old);
    };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void TracingCollector::EnumStaticRoots(RootSet& rootSet) const
{
    const RootSlotVisitor& visitor = [&rootSet, this](RootSlot& root) {
        if (VerifyRoots::Enabled()) {
            RootVerifyContext ctx;
            ctx.phase = "EnumStaticRoots";
            ctx.kind = RootKind::STATIC_ROOT;
            ctx.rawValue = raw(root.LoadPlain());
            ctx.hasRawValue = true;
            VerifyRoots::VerifyRootPayload(ctx, &root, nullptr);
        }
        EnumAndTagRawRoot(root, rootSet, Generation::Old);
    };
    VisitStaticRoots(visitor);
}

void TracingCollector::MergeMutatorRoots(WorkStack& workStack)
{
    (void)workStack;
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(majorMarkDomain.get());
}

void TracingCollector::EnumAllExportRoots(RootSet &foreignRootsSet)
{
    Heap::GetHeap().VisitAllExportRoots([&foreignRootsSet, this](ObjectRef& root) {
        if (VerifyRoots::Enabled()) {
            RootVerifyContext ctx;
            ctx.phase = "EnumAllExportRoots";
            ctx.kind = RootKind::RUNTIME_ROOT;
            ctx.rawValue = raw(root.LoadPlain());
            ctx.hasRawValue = true;
            VerifyRoots::VerifyRootPayload(ctx, &root, nullptr);
        }
        EnumAndTagRawRoot(root, foreignRootsSet, Generation::Old);
    });
}
void TracingCollector::DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    ScopedEntryTrace trace("CJRT_GC_ENUM");
    EnumAllCommonRoots(GetWorkers(), workStack);
    EnumAllExportRoots(foreignRootsSet);
}

void TracingCollector::AddExportObjectsTracingWork(RootSet &exportRoots)
{
    if (exportRoots.empty()) {
        return;
    }
    ExportRootsTracingWork task(*this, std::move(exportRoots));
    exportRoots.clear();
    GetWorkers().Run(task);
}

void TracingCollector::StartOldMarkWork()
{
    // ZGenerationOld::mark_start -> ZMark::start. Initialize the existing M3
    // domain before publishing old's mark phase to mutators and young workers.
    if (majorMarkDomain == nullptr) {
        majorMarkDomain = std::make_unique<MarkDomain>(64, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    }
    GCWorkers& workers = collectorResources.GetWorkers(GCCycleGeneration::OLD);
    workers.SetActiveWorkers(static_cast<uint32_t>(GetGCThreadCount(true)));
    majorMarkDomain->BindWorkers(&workers);
    majorMarkDomain->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMarkDomain->PrepareWork(workers.ActiveWorkers());
}

void TracingCollector::MarkOldObjectIfActive(BaseObject* object, bool gcThread) const
{
    const GCCycleSnapshot old = GetCycleSnapshot(GCCycleGeneration::OLD);
    if (!old.active || (old.phase != GC_PHASE_ENUM && old.phase != GC_PHASE_TRACE &&
                        old.phase != GC_PHASE_CLEAR_SATB_BUFFER)) {
        return;
    }
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    const bool marked = gcThread ? MarkObject(object) : IsMarkedObject<Generation::Old>(object);
    if (marked) {
        return;
    }
    // ZMark::mark_object marks before publishing GC-thread work. The entry
    // carries FollowOnly so old workers still traverse an already marked root.
    CHECK_DETAIL(majorMarkDomain != nullptr, "old mark domain must start before publication");
    MarkStripeSet& stripes = majorMarkDomain->Stripes();
    MarkThreadLocalStacks& publication = ThreadLocal::GetMarkStacks(*majorMarkDomain);
    publication.Push(stripes, stripes.StripeForAddress(reinterpret_cast<uintptr_t>(object)),
                     gcThread ? MarkStackEntry::FollowOnly(object) : MarkStackEntry::MarkAndFollow(object), true);
}

size_t TracingCollector::RunMajorStripeMark(WorkStack& workStack, bool partial)
{
    GCWorkers& workersSet = GetWorkers();
    const uint32_t workers = workersSet.ActiveWorkers();
    if (majorMarkDomain == nullptr) {
        majorMarkDomain = std::make_unique<MarkDomain>(64, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    }
    majorMarkDomain->BindWorkers(&workersSet);
    majorMarkDomain->BindAbort(&collectorResources.GetMajorDriverPort().Abort());
    majorMarkDomain->PrepareWork(workers);
    MajorMarkShared shared;
    shared.collector = this;
    shared.workerCount = workers;
    shared.partial = partial;
    shared.domain = majorMarkDomain.get();

    MarkThreadLocalStacks& seed = majorMarkDomain->Stacks();
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        seed.Push(shared.Stripes(), shared.StripeFor(entry), entry, true);
    }
    (void)seed.Flush(shared.Stripes(), true);
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                      VerifyMarkingStacks::MarkingContainer::TASK, shared.Stripes().Population());

    ConcurrentMarkingWork task(shared);
    workersSet.Run(task);
    majorMarkDomain->FinishWork();
    if (!partial && !collectorResources.GetMajorDriverPort().Abort().Poll()) {
        CHECK_DETAIL(shared.Terminate().Terminated(),
                     "major striped closure returned without coordinated worker termination");
    }
    return shared.newlyMarked.load(std::memory_order_relaxed);
}

void TracingCollector::TracingImpl(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                      VerifyMarkingStacks::MarkingContainer::OWNER, workStack.size());
    VerifyMarkingStacks::NoteProducer(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                      VerifyMarkingStacks::MarkingContainer::FOREIGN, foreignRootsSet.size());
    if (workStack.empty() && foreignRootsSet.empty() && majorMarkDomain->Stripes().IsEmpty()) {
        return;
    }

    if (!workStack.empty() || !majorMarkDomain->Stripes().IsEmpty()) {
        markedObjectCount.fetch_add(RunMajorStripeMark(workStack), std::memory_order_relaxed);
        VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                         VerifyMarkingStacks::MarkingBoundary::JOIN,
                                         VerifyMarkingStacks::MarkingContainer::STRIPE,
                                         0, VerifyMarkingStacks::NO_MARKING_INDEX,
                                         VerifyMarkingStacks::NO_MARKING_INDEX,
                                         VerifyMarkingStacks::NO_MARKING_INDEX);
    }
    AddExportObjectsTracingWork(foreignRootsSet);
    VerifyMarkingStacks::VerifyEmpty(VerifyMarkingStacks::MarkingGeneration::MAJOR,
                                     VerifyMarkingStacks::MarkingBoundary::JOIN,
                                     VerifyMarkingStacks::MarkingContainer::POOL,
                                     GetWorkers().GetSnapshot().remainingWorkers, 0);
}

void TracingCollector::FindUselessExternObjects()
{
    // DFSTraceExportObject publishes value-only roots after export tracing has
    // joined. Resolve and rekey that carrier before its first mark-bit read.
    // The producer lock remains the owner boundary; no field obligation is
    // introduced for these root referents.
    {
        std::lock_guard<std::mutex> lock(externMtx);
        CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
    }
    auto it = discoveredExternObjects.begin();
    while (it != discoveredExternObjects.end()) {
        auto& ls = it->second;
        auto listIt = ls.begin();
        auto listEnd = ls.end();
        while (listIt != listEnd) {
            if (IsMarkedObject<Generation::Old>(*listIt)) {
                listIt = ls.erase(listIt);
            } else {
                // MarkObject paints the live bit and nothing enqueues this object, so
                // its ref fields are never scanned this cycle: the mark closure has
                // already finished (DoTracing calls this after ConcurrentReMark). An
                // object kept alive here therefore keeps nothing else alive, and a
                // reference it holds can name an unmarked object -- which is what
                // MarkCompleteVerify reports as a DEAD_EDGE. Record what was painted so
                // that report can be joined against this list by address instead of
                // guessed at. Gated with the verifier; no cost when it is off.
                if (UNLIKELY(MarkCompleteVerify::Enabled())) {
                    LOG(RTLOG_ERROR, "[GCV2][markcomplete] EXTERN_PAINT_NO_FOLLOW obj=%p exportObj=%p",
                        static_cast<void*>(*listIt), static_cast<void*>(it->first));
                }
                MarkObject(*listIt);
                listIt++;
            }
        }
        it++;
    }
}
void TracingCollector::DoTracing(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    ScopedEntryTrace trace("CJRT_GC_TRACE");
    MRT_PHASE_TIMER("DoTracing");
    VLOG(REPORT, "roots size: %zu", workStack.size());

    {
        MRT_PHASE_TIMER("Concurrent marking");
        TracingImpl(workStack, foreignRootsSet);
    }

    {
        MRT_PHASE_TIMER("Concurrent re-marking");
        ConcurrentReMark(workStack);
    }

    // ZGenerationOld::collect processes non-strong references only after the
    // successful mark-end pause has closed ordinary mark publication.
    ProcessOldNonStrongReferences(workStack);

#if defined(MRT_TESTABLE_INTERNALS)
    // All major tasks and finalizer work have flushed before page selection.
    // Major has no reachableVec carrier; observers read the actual page state.
    ObserveMarkClosureForTest(nullptr);
#endif
    VLOG(REPORT, "mark %zu objects", markedObjectCount.load(std::memory_order_relaxed));
}

void TracingCollector::ProcessOldNonStrongReferences(WorkStack& workStack)
{
    CHECK_DETAIL(oldCycle.Phase() == GC_PHASE_MARK_COMPLETE,
                 "non-strong references require completed old marking");
    {
        MRT_PHASE_TIMER("identify useless extern ref");
        FindUselessExternObjects();
    }
    {
        // This explicit finalizable closure may mark after ordinary mark work
        // is closed, like ZGenerationOld::process_non_strong_references.
        MRT_PHASE_TIMER("concurrent resurrection");
        DoResurrection(workStack);
    }
    // Process the discovered references after the finalizable closure, before
    // relocation-set processing (zGeneration.cpp:1330-1335).
    ProcessFinalizers();
    // zGeneration.cpp:1344-1373: finish in-flight weak loads before unblocking.
    // A serial driver and synchronous GCWorkers::Run have already joined GC
    // work here; mutators (including the finalizer thread) need a rendezvous.
    MutatorManager::Instance().RunEpochHandshake("old non-strong references", false);
    collectorResources.UnblockResurrection();
    collectorResources.GetFinalizerProcessor().EnqueueReferences();
}

bool TracingCollector::FinishOldMark(WorkStack& workStack)
{
    // ZMark::end/try_end and ZGenerationOld::concurrent_mark_continue
    // (zMark.cpp:940-989; zGeneration.cpp:1015-1030).
    MarkStripeSet& stripes = majorMarkDomain->Stripes();
    for (;;) {
        if (!workStack.empty() || !stripes.IsEmpty()) {
            markedObjectCount.fetch_add(RunMajorStripeMark(workStack), std::memory_order_relaxed);
        }
        if (Heap::GetHeap().GetGCPhase() != GC_PHASE_CLEAR_SATB_BUFFER) {
            TransitionToGCPhase(GC_PHASE_CLEAR_SATB_BUFFER, true);
            if (!stripes.IsEmpty()) {
                continue;
            }
        }
        bool more = FlushMarkProducers(majorMarkDomain.get());
        if (more) {
            continue;
        }
        bool terminated;
        {
            ScopedStopTheWorld stw("old mark end", true, GC_PHASE_CLEAR_SATB_BUFFER);
            NoteMarkTerminatePause();
            const size_t before = stripes.Population();
            (void)MutatorManager::Instance().HandshakeFlushMarkProducers(majorMarkDomain.get());
            const size_t after = stripes.Population();
            NoteMarkTerminateFlushed(after >= before ? after - before : 0);
            terminated = workStack.empty() && stripes.IsEmpty();
            if (terminated) {
                // Publish while mutators are still stopped. Ordinary mark_if_active
                // producers must close before non-strong references are processed.
                // Keep the existing barrier installed until POST_TRACE; marking
                // admission is owned by this generation, not the barrier variant.
                oldCycle.PublishPhase(GC_PHASE_MARK_COMPLETE);
                // zGeneration.cpp:1280: close weak resurrection in the same pause.
                collectorResources.BlockResurrection();
            }
        }
        if (terminated) {
            ReportMarkTerminateContinue();
            return true;
        }
        NoteMarkTerminateContinue(workStack.size() + stripes.Population());
    }
}

void TracingCollector::ConcurrentReMark(WorkStack& remarkStack)
{
    CHECK_DETAIL(FinishOldMark(remarkStack), "not cleared\n");
}

bool TracingCollector::FlushMarkProducers(MarkDomain* domain)
{
    bool flushed = domain != nullptr ? domain->TryTerminateFlush() :
        MutatorManager::Instance().HandshakeFlushMarkProducers(nullptr);
    if (domain != nullptr) {
        flushed = domain->FlushStacks() || flushed || !domain->Stripes().IsEmpty();
    }
    return flushed;
}

void TracingCollector::DoResurrection(WorkStack& workStack)
{
    workStack.clear();
    RootVisitor func = [&workStack, this](ObjectRef& ref) {
        HeapSlot<> tmpField(to_zpointer(raw(ref.LoadPlain())));
        BaseObject* finalizerObj = to_object(tmpField.GetTargetObject());
        if (!IsMarkedObject<Generation::Old>(finalizerObj)) {
            DLOG(TRACE, "resurrectable obj @%p:%p", &ref, finalizerObj);
            CHECK(DiscoverReference(finalizerObj, ReferenceType::FINAL) == ReferenceStatus::DISCOVERED);
            workStack.push_back(MarkStackEntry::MarkAndFollow(finalizerObj, true));
        }
        if (raw(ref.LoadPlain()) != reinterpret_cast<MAddress>(finalizerObj)) {
            HealRoot(ref, from_object(finalizerObj), HealSite::TracingCollectorResurrectFinalizer);
            DLOG(FIX, "heal finalizer %p@%p", finalizerObj, &ref);
        }
    };
    (void)collectorResources.GetFinalizerProcessor().VisitFinalizers(func);

    size_t resurrectdObjects = 0;
    MarkLiveCache liveCache(1);
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();

        // TraceObjectRefFields below can push partial-array chunks onto this
        // stack too, so this loop has to decode them as well.
        if (UNLIKELY(MarkPartialArray::IsPartialArrayEntry(entry))) {
            FollowPartialArray(entry, workStack);
            continue;
        }
        BaseObject* obj = entry.object();

        if (MarkEntryObject(obj, entry, &liveCache)) {
            continue;
        }
        if (entry.mark()) {
            ++resurrectdObjects;
        }
        if (entry.follow() && obj->HasRefField()) {
            TraceObjectRefFields(obj, workStack, entry.finalizable());
        }
    }
    liveCache.Flush();
    markedObjectCount.fetch_add(resurrectdObjects, std::memory_order_relaxed);
    VLOG(REPORT, "resurrected objects %zu", resurrectdObjects);
}

bool TracingCollector::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                                       MarkLiveCache* cache) const
{
    if (!Collector::PlausibleManagedObjectGate("MarkEntryObject", obj)) {
        return true;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    bool firstLive = entry.incLive();
    bool already = false;
    if (entry.mark()) {
        if (entry.finalizable()) {
            already = region->ResurrectObjectWithLiveClaim(
                obj, region->GetAddressOffset(reinterpret_cast<MAddress>(obj)), false, firstLive);
        } else {
            already = region->MarkObjectByOwnerWithLiveClaim(obj, obj->GetSize(), false, firstLive);
        }
    }
    if (!already && firstLive) {
        if (cache != nullptr) {
            cache->IncLive(region, obj->GetSize());
        } else {
            region->AddLiveCounts(1, obj->GetSize());
        }
    }
    return already;
}

void TracingCollector::Init() {}

void TracingCollector::Fini() { Collector::Fini(); }

BaseObject* TracingCollector::ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
                                                      ForwardingStage stage) const
{
    if (value == nullptr || !Heap::IsHeapAddress(value)) {
        return value;
    }
    const ForwardingProvenance provenance{
        ForwardingHolderKind::Static, owner, nullptr, stage, ForwardingWriterKind::CollectorHeal,
        ForwardingSourceKind::CallerValue, nullptr, nullptr, ForwardingFieldKind::RootSlot
    };
    BaseObject* current = ResolveStoreValue(value, provenance, generation);
    CHECK_DETAIL(current != nullptr && Heap::IsHeapAddress(current),
                 "value root resolve requires a heap to-address from=%p current=%p", value, current);
    CHECK_DETAIL(Collector::JudgeHandOutTarget(current) == HandVerdict::Usable,
                 "value root resolve requires a usable target from=%p current=%p", value, current);
    return current;
}

void TracingCollector::CurrentizeValueRootSet(std::unordered_set<BaseObject*>& roots, Generation generation) const
{
    std::unordered_set<BaseObject*> current;
    current.reserve(roots.size());
    for (BaseObject* value : roots) {
        current.insert(ResolveCurrentValueRoot(value, &roots, generation));
    }
    roots.swap(current);
}

void TracingCollector::CurrentizeValueRootMap(
    std::unordered_map<BaseObject*, std::list<BaseObject*>>& roots, Generation generation) const
{
    std::unordered_map<BaseObject*, std::list<BaseObject*>> current;
    current.reserve(roots.size());
    for (const auto& entry : roots) {
        BaseObject* key = ResolveCurrentValueRoot(entry.first, &roots, generation);
        std::list<BaseObject*>& values = current[key];
        for (BaseObject* value : entry.second) {
            values.push_back(ResolveCurrentValueRoot(value, &roots, generation));
        }
    }
    roots.swap(current);
}

// Registered finalizers are discovered by DoResurrection and fixed by
// VisitRawPointers. Only queued/running finalizables are strong mark roots.
void TracingCollector::EnumFinalizerProcessorRoots(RootSet& rootSet) const
{
    RootVisitor visitor = [this, &rootSet](ObjectRef& root) {
        if (VerifyRoots::Enabled()) {
            RootVerifyContext ctx;
            ctx.phase = "EnumFinalizerProcessorRoots";
            ctx.kind = RootKind::RUNTIME_ROOT;
            ctx.rawValue = raw(root.LoadPlain());
            ctx.hasRawValue = true;
            VerifyRoots::VerifyRootPayload(ctx, &root, nullptr);
        }
        EnumAndTagRawRoot(root, rootSet, Generation::Old);
    };
    collectorResources.GetFinalizerProcessor().VisitGCRoots(visitor);
}

void TracingCollector::EnumAllSurrectedExportRoots(RootSet &rootSet)
{
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Old);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Old);
        for (auto* obj : resurrectedExportObjectes) {
            rootSet.push_back(obj);
        }
        for (auto* obj : resurrectedExportObjectesForwardPhase) {
            rootSet.push_back(obj);
        }
    }
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
    auto it = cycleRefWorkStack.begin();
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        rootSet.push_back(exportObj);
        for (auto &externObj : it->second) {
            rootSet.push_back(externObj);
        }
        it++;
    }
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void TracingCollector::DumpHeap(const CString& tag)
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "Not In STW");
    DLOG(FRAGMENT, "DumpHeap %s", tag.Str());
    // dump roots
    DumpRoots(FRAGMENT);
    // dump object contents
    auto dumpVisitor = [](BaseObject* obj) { obj->DumpObject(FRAGMENT); };
    bool ret = Heap::GetHeap().ForEachObj(dumpVisitor, false);
    CHECK_E(UNLIKELY(!ret), "theAllocator.ForEachObj() in DumpHeap() return false.");

    // dump object types
    DLOG(FRAGMENT, "Print Type information");
    std::set<TypeInfo*> classinfoSet;
    auto assembleClassInfoVisitor = [&classinfoSet](BaseObject* obj) {
        TypeInfo* classInfo = obj->GetTypeInfo();
        // No need to check the result of insertion, because there are multiple-insertions.
        (void)classinfoSet.insert(classInfo);
    };
    ret = Heap::GetHeap().ForEachObj(assembleClassInfoVisitor, false);
    CHECK_E(UNLIKELY(!ret), "theAllocator.ForEachObj()#2 in DumpHeap() return false.");

    for (auto it = classinfoSet.begin(); it != classinfoSet.end(); it++) {
        TypeInfo* classInfo = *it;
        DLOG(FRAGMENT, "%p %s", classInfo, classInfo->GetName());
    }
    DLOG(FRAGMENT, "Dump Allocator");
}

ATTR_NO_SANITIZE_ADDRESS
void TracingCollector::DumpRoots(LogType logType)
{
    RootVisitor rootVisitor = [this, logType](ObjectRef& ref) {
        zaddress_unsafe value = ref.LoadPlain();
        if (is_null(value)) {
            return;
        }
        // DumpRoots is called while the root owner retains the target for inspection.
        auto obj = to_object(safe(value));
        DLOG(logType, "%p Fast Check %d Accurate Check %d", obj,
             theAllocator.IsHeapAddress(reinterpret_cast<MAddress>(obj)),
             theAllocator.IsHeapObject(reinterpret_cast<MAddress>(obj)));
    };

    DLOG(logType, "stack roots");
    MutatorManager::Instance().VisitAllMutators(
        [&rootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rootVisitor); });

    DLOG(logType, "finalizer processor roots");
    VisitFinalizerRoots(rootVisitor);

    RootSlotVisitor rootSlotVisitor = [this, logType](RootSlot& ref) {
        zaddress_unsafe value = ref.LoadPlain();
        if (is_null(value)) {
            return;
        }
        // StaticRootTable keeps the referent live while DumpRoots inspects it.
        auto obj = to_object(safe(value));
        if (obj == nullptr) {
            return;
        }
        DLOG(logType, "%p Fast Check %d Accurate Check %d", obj,
             theAllocator.IsHeapAddress(reinterpret_cast<MAddress>(obj)),
             theAllocator.IsHeapObject(reinterpret_cast<MAddress>(obj)));
    };

    DLOG(logType, "static fields");
    VisitStaticRoots(rootSlotVisitor);

    DLOG(logType, "Dump GCRoots end");
}
#endif

void TracingCollector::PreGarbageCollection(bool isConcurrent, uint64_t gcIndex)
{
    const bool continuingPrelude = ActiveCycle().Snapshot().active;
    if (!continuingPrelude) {
        ActiveCycle().Begin(gcIndex);
    }
    ResetSkippedStackMapCounts();
    VLOG(REPORT, "Begin GC log. GCReason: %s, Current allocated %s, Current threshold %s, current tag %u",
         g_gcRequests[GetCycleReason()].name, Pretty(Heap::GetHeap().GetAllocatedSize()).Str(),
         Pretty(Heap::GetHeap().GetCollector().GetGCStats().GetThreshold()).Str(),
         static_cast<unsigned>(GetCurrentTagID()));

    // zDriver.cpp:183,399-400: generation workers use their concurrent
    // budget for both pause and concurrent work. Parallel workers are separate.
    const int32_t threadCount = GetGCThreadCount(true);
    GetWorkers().SetActive();
    GetWorkers().SetActiveWorkers(static_cast<uint32_t>(threadCount));
    VLOG(REPORT, "GC generation active workers: %d", threadCount);

    GetGCStats().reason = GetCycleReason();
    GetGCStats().async = (gcIndex == GCTask::ASYNC_TASK_INDEX);
    GetGCStats().isConcurrentMark = isConcurrent;
#if defined(MRT_TESTABLE_INTERNALS)
    if (testCyclePrepared) {
        testCyclePrepared();
    }
#endif
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpBeforeGC();
#endif
    TRACE_COUNT("CJRT_pre_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

void TracingCollector::PostGarbageCollection(uint64_t gcIndex)
{
    GetWorkers().SetInactive();
    // Periodic persistence: timeout/ABRT/SIGKILL cannot erase counters from
    // completed GC cycles. Both probes self-gate and remain default off.
    // holdercapture: periodic persistence, so ABRT/kill cannot erase the snapshot census.

    MarkCompleteVerify::ReportHolderTraces("gc_end");
    // loadgood: same reason -- the workload under measurement ends in SIGSEGV, so the
    // cross-table has to be on stderr before the crash, not only at exit.

    // portarray: positive control for large-array chunking; self-gates, default off.
    MarkPartialArray::Report("gc_end");
    ReportSkippedStackMapCounts();
    // release pages in PagePool
    TransitionToGCPhase(GCPhase::GC_PHASE_RECLAIM_SATB_NODE, true);
    NwDropAudit::Report("reclaim_satb");
    PagePool::Instance().Trim();
    (void)gcIndex;

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpAfterGC();
#endif
}

void TracingCollector::DFSTraceExportObject(BaseObject *exportObj, bool finalizable)
{
    WorkStack workStack;
    workStack.push_back(MarkStackEntry::FollowOnly(exportObj, finalizable));
    std::list<BaseObject*> externObjs;
    MarkLiveCache liveCache(1);
    BaseObject* obj = nullptr;
    auto visit = [&workStack, &obj, this, &externObjs, &liveCache, &finalizable](MAddress slot) {
            RefField<>& field = HeapSlotAt<>(slot);
            RefField<> oldField(field);
            // mark-good fast path (zcolor2 @ 84a64e88): already passed this mark epoch.
            if (is_mark_good(oldField)) {
                BaseObject* targetObj = to_object(oldField.GetTargetObject());
                if (!Collector::MarkGoodHeapGate("DFSTraceExportObject", targetObj)) {
                    return;
                }
                if (IsMarkedObject<Generation::Old>(targetObj)) {
                    return;
                }
                if (targetObj->GetTypeInfo()->IsForeignType()) {
                    workStack.push_back(MarkStackEntry::FollowOnly(targetObj, finalizable));
                    externObjs.push_back(targetObj);
                } else if (!MarkEntryObject(targetObj, MarkStackEntry::MarkAndFollow(targetObj, finalizable), &liveCache)) {
                    workStack.push_back(MarkStackEntry::FollowOnly(targetObj, finalizable));
                }
                return;
            }

            // Slow path: load-good + generation route (OpenJDK ZBarrier::make_load_good),
            // then recolour with current mark/remap. Replaces IsOldPointer/FindLatestVersion.
            const ForwardingProvenance provenance{ ForwardingHolderKind::HeapRef, obj, &field };
            BaseObject* latest = make_load_good(oldField, provenance);

            // target object could be null or non-heap for some static variable.
            if (!Heap::IsHeapAddress(latest)) {
                return;
            }
            CHECK(latest->IsValidObject());

            RefField<> newField = GetAndTryTagRefField(latest);
            if (oldField.GetFieldValue() == newField.GetFieldValue()) {
                DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(),
                     latest->GetSize());
            } else if (HealSlot(field, oldField.GetFieldValue(), newField.GetFieldValue(),
                                HealSite::TracingCollectorTraceRefField)) {
                DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
                     raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
            }

            if (IsMarkedObject<Generation::Old>(latest)) {
                return;
            }
            if (latest->GetTypeInfo()->IsForeignType()) {
                workStack.push_back(MarkStackEntry::FollowOnly(latest, finalizable));
                externObjs.push_back(latest);
            } else {
                if (!MarkEntryObject(latest, MarkStackEntry::MarkAndFollow(latest, finalizable), &liveCache)) {
                    workStack.push_back(MarkStackEntry::FollowOnly(latest, finalizable));
                }
            }
    };
    auto publish = [&workStack](const MarkStackEntry& entry) { workStack.push_back(entry); };
    while (!workStack.empty()) {
        const MarkStackEntry entry = workStack.back();
        workStack.pop_back();
        finalizable = entry.finalizable();
        if (entry.partialArray()) {
            obj = nullptr;
            MarkPartialArray::FollowPartialReferences(entry, visit, publish);
            continue;
        }
        obj = entry.object();
        if (!entry.follow()) {
            continue;
        }
        if (UNLIKELY(obj->IsWeakRef())) {
            DiscoverWeakReference(obj, workStack);
            continue;
        }
        MarkPartialArray::FollowObjectReferences(obj, finalizable, visit, publish);
    }
    liveCache.Flush();
    std::lock_guard<std::mutex> lg(externMtx);
    discoveredExternObjects[exportObj] = externObjs;
}
void TracingCollector::EnumAllCommonRoots(GCWorkers& workers, RootSet& rootSet)
{
    // zRootsIterator.cpp: generation workers claim independent root families.
    const uint32_t count = workers.ActiveWorkers();
    std::vector<RootSet> roots(count);
    std::atomic<unsigned> next { 0 };
    class RootsTask final : public GCWorkerTask {
    public:
        explicit RootsTask(std::function<void(uint32_t)> body) : body(std::move(body)) {}
        void Work(uint32_t id) override { body(id); }
    private:
        std::function<void(uint32_t)> body;
    } task([&](uint32_t id) {
        for (unsigned family = next.fetch_add(1); family < 4; family = next.fetch_add(1)) {
            switch (family) {
                case 0: EnumStaticRoots(roots[id]); break;
                case 1: EnumConcurrencyModelRoots(roots[id]); break;
                case 2: EnumFinalizerProcessorRoots(roots[id]); break;
                case 3: EnumAllSurrectedExportRoots(roots[id]); break;
            }
        }
    });
    workers.Run(task);
    MergeMutatorRoots(rootSet);
    for (auto& result : roots) {
        rootSet.insert(result);
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (testRootsResult) {
        testRootsResult(workers.GetSnapshot().generation, rootSet);
    }
#endif
    VLOG(REPORT, "Total roots: %zu(exclude stack roots)", rootSet.size());
}

void TracingCollector::VisitStaticRoots(const RootSlotVisitor& visitor) const
{
    Heap::GetHeap().VisitStaticRoots(visitor);
}

void TracingCollector::VisitFinalizerRoots(const RootVisitor& visitor) const
{
    collectorResources.GetFinalizerProcessor().VisitGCRoots(visitor);
}

void TracingCollector::UpdateGCStats()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& gcStats = GetGCStats();
    gcStats.Dump();

    size_t oldThreshold = gcStats.GetThreshold();
    size_t liveBytes = space.AllocatedBytes();
    size_t heapSize = space.GetMaxCapacity();
    size_t recentBytes = space.GetRecentAllocatedSize();

    // 2 / 3: when live bytes is over 2/3 heap size, the async allocation need to be closed.
    if (liveBytes > heapSize * 2 / 3) {
        space.EnableAsyncAllocation(false);
    } else {
        space.EnableAsyncAllocation(true);
    }
    // 4 ways to estimate heap next threshold.
#if defined (__OHOS__)
    constexpr double lowUtilGrowth = 1.8;
    constexpr double lowUtilRatio = 0.25;
    double heapGrowth = liveBytes < heapSize * lowUtilRatio ?
        lowUtilGrowth : 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#else
    double heapGrowth = 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#endif
    size_t threshold1 = static_cast<size_t>(liveBytes * heapGrowth);
    size_t threshold2 = static_cast<size_t>(oldThreshold * heapGrowth);
    size_t threshold3 = static_cast<size_t>(liveBytes * 1.2 / (1.0 + gcStats.garbageRatio));
    size_t threshold4 = space.GetTargetSize();
    size_t newThreshold = 0;
    uint64_t gcInterval = CangjieRuntime::GetGCParam().gcInterval;
    // 2 : We regard the half of heap size as a limit because of copying algorithm.
    if (liveBytes < oldThreshold && oldThreshold < (heapSize / 2)) {
#if defined (__OHOS__)
        // When the ulitization is low, we can give the old threshold a larger weight to compute average value.
        // 1, 4, 2, 1: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 1 + threshold2 * 4 + threshold3 * 2 + threshold4 * 1) / 8;
        // 2s: We set the max waiting time to 2s to avoid memory increasing too fast.
        auto maxAdaptiveInterval = static_cast<uint64_t>(2) * MapleRuntime::SECOND_TO_NANO_SECOND;
        uint64_t gcAdaptiveInterval = maxAdaptiveInterval;
        if (gcStats.collectionRate > 0.0) {
            double estimatedInterval = static_cast<double>(newThreshold - liveBytes) / MB /
                gcStats.collectionRate * MapleRuntime::SECOND_TO_NANO_SECOND;
            gcAdaptiveInterval = static_cast<uint64_t>(
                std::min(estimatedInterval, static_cast<double>(maxAdaptiveInterval)));
        }
        gcInterval = std::max(gcInterval, gcAdaptiveInterval);
#else
        // 4: Computing arithmetic mean
        newThreshold = (threshold1 + threshold2 + threshold3 + threshold4) / 4;
#endif
    } else {
        // When the ulitization is high, we try to avoid threshold increasing and give it a small weight.
        // 2, 1, 2, 3: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 2 + threshold2 * 1 + threshold3 * 2 + threshold4 * 3) / 8;
    }
    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(space.GetMaxCapacity() * 0.98));
    gcStats.heapThreshold.store(std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold),
                                std::memory_order_release);
    g_gcRequests[GC_REASON_HEU].SetMinInterval(gcInterval);
    VLOG(REPORT, "live bytes %zu (survived %zu, recent-allocated %zu), update gc threshold %zu -> %zu", liveBytes,
         liveBytes - recentBytes, recentBytes, oldThreshold, gcStats.GetThreshold());
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}
} // namespace MapleRuntime
