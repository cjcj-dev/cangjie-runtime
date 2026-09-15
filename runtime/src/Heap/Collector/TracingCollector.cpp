// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
// Read-only observation after root workers return and before old follow starts.
// Published nodes are immutable; producers may prepend but no worker removes
// nodes during this phase. This copies actual product publication, not inputs.
struct RootPublicationSnapshot {
    static void CopyList(const MarkStripeStackList& list, TracingCollector::RootSet& result)
    {
        for (auto* node = list.head.load(std::memory_order_acquire); node != nullptr; node = node->Next()) {
            const auto* stack = node->Stack();
            for (size_t i = 0; i < stack->top; ++i) {
                const auto entry = stack->entries(stack)[i];
                if (!entry.partial_array()) { result.push_back(entry); }
            }
        }
    }
    static void Copy(MarkDomain& domain, TracingCollector::RootSet& result)
    {
        for (size_t i = 0; i < domain.Stripes().Count(); ++i) {
            const auto& stripe = domain.Stripes().At(i);
            CopyList(stripe.published, result);
            CopyList(stripe.overflowed, result);
        }
    }
};

void TracingCollector::ObservePublishedRoots(GCWorkers::Generation generation)
{
    if (testRootsResult) {
        RootSet published;
        RootPublicationSnapshot::Copy(*majorMarkDomain, published);
        testRootsResult(generation, published);
    }
}

std::function<void(GCWorkers::Generation, TracingCollector::RootSet&)> TracingCollector::testRootsResult;
std::function<void(GCWorkers::Generation, NativeSlot*)> TracingCollector::testColoredRootResult;
std::function<void()> TracingCollector::testCyclePrepared;
std::function<void()> TracingCollector::testYoungMarkStarted;
std::function<void()> TracingCollector::testOldMarkStarted;
std::function<void(GCCycleGeneration, MarkStartPoint, const MarkDomain*)> TracingCollector::testMarkStartState;
std::function<void()> TracingCollector::testYoungMarkCompleted;
std::function<void(const ExportOwnershipTestObservation&)> TracingCollector::testExportOwnershipResult;
#endif

// ZMark::_ncontinue (zMark.cpp:975-981). Always on so a zero is readable as
// "the pre-pause test was right every time" rather than "nobody is counting".
std::atomic<size_t> g_markTerminateContinue{ 0 };
std::atomic<size_t> g_markTerminatePauses{ 0 };
std::atomic<size_t> g_markTerminateFlushed{ 0 };
std::atomic<bool> g_markTerminateAtexitInstalled{ false };
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<uint64_t> g_markTerminateMaxPauseNs{ 0 };
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
    g_markTerminatePauseY2y.store(0, std::memory_order_relaxed);
    g_markTerminateClosureDuringPause.store(0, std::memory_order_relaxed);
}

MarkTerminateTestReceipt ReadMarkTerminateTestReceipt()
{
    return { g_markTerminatePauses.load(std::memory_order_relaxed),
             g_markTerminateFlushed.load(std::memory_order_relaxed),
             g_markTerminateContinue.load(std::memory_order_relaxed),
             g_markTerminateMaxPauseNs.load(std::memory_order_relaxed),
             g_markTerminatePauseY2y.load(std::memory_order_relaxed),
             g_markTerminateClosureDuringPause.load(std::memory_order_relaxed) };
}

void NoteMarkTerminatePauseDuration(uint64_t pauseNs)
{
    uint64_t observed = g_markTerminateMaxPauseNs.load(std::memory_order_relaxed);
    while (observed < pauseNs &&
           !g_markTerminateMaxPauseNs.compare_exchange_weak(observed, pauseNs, std::memory_order_relaxed)) {}
}

void NoteMarkTerminatePauseProducers(size_t y2y)
{
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

    NativeSlotVisitor rootSlotVisitor = [this, logType](NativeSlot& ref) {
        zpointer value = ref.GetFieldValue();
        if (is_null(value)) {
            return;
        }
        // StaticRootTable keeps the referent live while DumpRoots inspects it.
        auto obj = Heap::GetBarrier().ReadStaticRef(ref);
        if (obj == nullptr) {
            return;
        }
        DLOG(logType, "%p Fast Check %d Accurate Check %d", obj,
             theAllocator.IsHeapAddress(reinterpret_cast<MAddress>(obj)),
             theAllocator.IsHeapObject(reinterpret_cast<MAddress>(obj)));
    };

    DLOG(logType, "static fields");
    VisitFinalizerRoots(rootSlotVisitor);
    VisitStaticRoots(rootSlotVisitor);

    DLOG(logType, "Dump GCRoots end");
}
#endif

} // namespace MapleRuntime

namespace MapleRuntime {
namespace {
struct SkippedStackMapCounts {
    std::atomic<size_t> zeroEntries{ 0 };
    std::atomic<size_t> pcMiss{ 0 };
    std::atomic<size_t> zeroRootIndices{ 0 };
};

SkippedStackMapCounts g_skippedStackMapCounts;
thread_local size_t g_currentThreadRootMapMissCount = 0;

} // namespace

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

ATTR_NO_INLINE void RecordSkippedStackMap(StackMapInvalidReason reason, const FrameInfo&, uintptr_t,
                                          uintptr_t)
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
size_t TracingCollector::CurrentThreadRootMapMissCount()
{
    return g_currentThreadRootMapMissCount;
}



}

namespace MapleRuntime {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void TracingCollector::DumpBeforeGC()
    {
        if (ENABLE_LOG(FRAGMENT)) {
            if (MutatorManager::Instance().WorldStopped()) {
                DumpHeap("before_gc");
            } else {
                ScopedStopTheWorld stw("dump before gc");
                DumpHeap("before_gc");
            }
        }
    }

void TracingCollector::DumpAfterGC()
    {
        if (ENABLE_LOG(FRAGMENT)) {
            if (MutatorManager::Instance().WorldStopped()) {
                DumpHeap("after_gc");
            } else {
                ScopedStopTheWorld stw("dump after gc");
                DumpHeap("after_gc");
            }
        }
    }
#endif
}

namespace MapleRuntime {
#ifdef MRT_TESTABLE_INTERNALS
USize StaticRootTable::RootCountForTesting()
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    return totalRootsCount;
}
#endif


} // namespace MapleRuntime
