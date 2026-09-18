// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <thread>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#endif

#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zStringDedup.hpp"
#include "Heap/z/zThread.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
ZCollectedHeap* ZCollectedHeap::heap()
{
    // Universe::initialize_heap creates the collector after VM/platform
    // initialization (universe.cpp:962-963, zArguments.cpp:243-245).
    // A DSO constructor here would observe dynamic page-size globals before
    // their initialization, depending on static archive link order.
    static ImmortalWrapper<ZCollectedHeap> collected;
    return &*collected;
}

ZCollectedHeap::ZCollectedHeap()
    : _heap(),
      _driver_minor(nullptr),
      _driver_major(nullptr),
      _director(nullptr),
      _stat(nullptr),
      _runtime_workers()
{
}

ZCollectedHeap::~ZCollectedHeap() = default;

void ZCollectedHeap::initialize_gc()
{
    ZAbort::reset();
    ZStat::Initialize();
    _heap.GetGCStats(ZGenerationId::young).Init();
    _heap.GetGCStats(ZGenerationId::old).Init();
    ZStatMutatorAllocRate::initialize();
    const uint64_t now = TimeUtil::NanoSeconds();
    _heap.young().CycleStats().Initialize(now);
    _heap.old().CycleStats().Initialize(now);
    _stat = new ZStat();
    start_gc_threads();
    _finalizer_processor.Start();
    StringDedup::Instance().Start();
    if (Uncommitter::Enabled()) {
        LOG(RTLOG_INFO, "Uncommit: Enabled delay=%zus",
            static_cast<size_t>(Uncommitter::DelayNs() / SECOND_TO_NANO_SECOND));
    } else {
        LOG(RTLOG_INFO, "Uncommit: Disabled");
    }
}

void ZCollectedHeap::finalize_gc()
{
    MRT_ASSERT(!_finalizer_processor.IsRunning(), "Invalid finalizerProcessor status");
    MRT_ASSERT(!_gc_thread_running.load(std::memory_order_relaxed), "Invalid GC thread status");
}

void ZCollectedHeap::start_gc_threads()
{
    bool expected = false;
    if (!_gc_thread_running.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        return;
    }
    if (_heap.young().Workers() == nullptr) {
        unsigned int activeProcessorCount = std::thread::hardware_concurrency();
        bool affinityDetected = false;
#if defined(__linux__) || defined(hongmeng)
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        if (sched_getaffinity(0, sizeof(cpuSet), &cpuSet) == 0) {
            int affinityProcessorCount = CPU_COUNT(&cpuSet);
            if (affinityProcessorCount > 0) {
                activeProcessorCount = static_cast<unsigned int>(affinityProcessorCount);
                affinityDetected = true;
            }
        }
#endif
        activeProcessorCount = std::max(activeProcessorCount, 1U);
        const size_t maxHeap = _heap.GetMaxCapacity();
        const auto& regions = static_cast<RegionSpace&>(_heap.GetAllocator()).GetRegionManager();
        const size_t regionBytes = regions.GetThreadLocalRegionSize();
        CHECK_DETAIL(regionBytes != 0, "worker region budget must be initialized");
        const size_t heapWorkers = maxHeap / 50 / regionBytes;
        const uint64_t cpus = activeProcessorCount;
        _concurrent_gc_threads = static_cast<int32_t>(std::max<size_t>(1,
            std::min<size_t>((cpus + 3) / 4, heapWorkers)));
        ConcGCThreads = static_cast<uint32_t>(_concurrent_gc_threads);
        ZYoungGCThreads = ConcGCThreads;
        ZOldGCThreads = ConcGCThreads;
        VLOG(REPORT,
             "concurrent gc thread count %d, active processor count %u, affinity detected %d, region bytes %zu",
             _concurrent_gc_threads, activeProcessorCount, affinityDetected, regionBytes);

        _heap.young().InitializeWorkers(_concurrent_gc_threads);
        _heap.old().InitializeWorkers(_concurrent_gc_threads);
        _finalizer_processor.GetReferenceProcessor().set_workers(_heap.old().Workers());
    }

    // The ImmortalWrapper constructs the heap before its size is known; start
    // drivers only after Heap::Init has installed the page table and workers.
    _driver_minor = new ZDriverMinor();
    _driver_major = new ZDriverMajor();
    _director = new ZDirector();
    _driver_minor->start();
    _driver_major->start();
}



void ZCollectedHeap::collect(GCReason reason, bool async)
{
    CHECK(reason < GC_REASON_MAX);
    if (!_heap.IsGCEnabled()) return;
    if (reason == GC_REASON_WB_BREAKPOINT) {
        _driver_major->collect(ZDriverRequest(reason, 0, 0));
        return;
    }
    ZDriverPort& port = reason == GC_REASON_YOUNG
        ? _driver_minor->port() : _driver_major->port();
    const ZDriverRequest request(reason, 0, 0);
    if (async) {
        CHECK(!g_gcRequests[reason].IsSyncGC());
        port.send_async(request);
    } else {
        ScopedEnterSaferegion enterSaferegion(false);
        port.send_sync(request);
    }
}

void ZCollectedHeap::stop()
{
    ZAbort::abort();
    ZCollectedHeap* collected = heap();
    if (collected->_finalizer_processor.IsRunning()) {
        collected->_finalizer_processor.Stop();
    }
    if (collected->_gc_thread_running.load(std::memory_order_acquire)) {
        for (ZThread* thread : { static_cast<ZThread*>(collected->_director),
                                 static_cast<ZThread*>(collected->_driver_major),
                                 static_cast<ZThread*>(collected->_driver_minor) }) {
            thread->stop();
        }
        delete collected->_director;
        delete collected->_driver_minor;
        delete collected->_driver_major;
        collected->_director = nullptr;
        collected->_driver_minor = nullptr;
        collected->_driver_major = nullptr;
        Heap::GetHeap().young().StopWorkers();
        Heap::GetHeap().old().StopWorkers();
        collected->_gc_thread_running.store(false, std::memory_order_release);
    }
    if (collected->_stat != nullptr) {
        collected->_stat->stop();
        delete collected->_stat;
        collected->_stat = nullptr;
    }
    StringDedup::Instance().Stop();
}

} // namespace MapleRuntime
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zMark.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {




namespace {


// zc7fix: is_mark_good fast path may admit plain non-heap slots (g_cjMarkBadMask all-zero on
// uncoloured non-null). Count rejects before IsValidObject/IsMarkedObject.


HandVerdict ClassifyRawHeader(uint64_t header)
{
    if (((header >> 48) & 0x3u) == 3u) {
        return HandVerdict::Forwarded;
    }
    if ((header & 0xffffffffffffull) == 0) {
        return HandVerdict::ZeroHeader;
    }
    return HandVerdict::Usable;
}

const char* HandVerdictName(HandVerdict verdict)
{
    switch (verdict) {
        case HandVerdict::Forwarded: return "Forwarded";
        case HandVerdict::ZeroHeader: return "ZeroHeader";
        case HandVerdict::Usable: return "Usable";
    }
    return "Unknown";
}

const char* ToAnswerName(int)
{
    return "table";
}

} // namespace

// F5: when FindToVersion returns null, never silently hand back a dead/zeroed from.
// Legal null (high-live / raw-pin survivor still at from, ghost=0) keeps returning obj.
// Illegal null (D: old tag + ghost already dispelled + from cleared) fails loudly here.
// See reports/REPORT-nullenum.md LEGAL_NULL_SET; reports/REPORT-tagaba.md F5.
// Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
// Synchronous root operations consume the generation of their phase context.
// Unlike ZStackWatermark, these closures do not retain frames across relocations.
BaseObject* HeapGcState::ValidateCurrentValue(BaseObject* ref, const ForwardingProvenance& provenance) const
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref) || JudgeHandOutTarget(ref) == HandVerdict::Usable) {
        return ref;
    }
    FailClosedLoad("current raw value required", ref, 0, provenance);
}



BaseObject* HeapGcState::FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance, Generation generation) const
{
    if (obj == nullptr) {
        return nullptr;
    }

    BaseObject* to = FindToVersion(obj, generation).GetOrFailClosed("HeapGcState::FindLatestVersion", provenance);
    if (to != nullptr) {
        if (to != obj && Heap::IsHeapAddress(to) && !to->IsValidObject()) {
            CHECK_DETAIL(obj->IsValidObject(),
                         "FindLatestVersion: route dest %p has no tip and from %p is not valid",
                         to, obj);
            return obj;
        }
        return to;
    }
    CHECK_DETAIL(obj->IsValidObject(),
                 "FindLatestVersion: no to-version for invalid from-object %p "
                 "(stale old-tag after ghost dispel; do not fall back to from)",
                 obj);
    return obj;
}

// loadfc: best-effort detection verdict. Same header-word shape as Barrier.cpp's former staleguard
// judge (StateWord.h:215-228: bits 0-47 TypeInfo, bits 48-49 stateCode; FORWARDED=3). This one
// relaxed read classifies the observed word; it does not establish object lifetime or happens-before.
HandVerdict HeapGcState::JudgeHandOutTarget(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return HandVerdict::Usable;
    }
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    return ClassifyRawHeader(hdr);
}


// loadfc (zBarrier.inline.hpp:327-343): the slow path must produce a verified current version or
// stop the mutator in a controlled, attributable place -- never hand back a structurally dead
// from-address. The [LOADFC] tag is the population-accounting signature.
[[noreturn]] void HeapGcState::FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                            const ForwardingProvenance& provenance)
{
    const HandVerdict verdict = JudgeHandOutTarget(target);
    const MAddress from = target != nullptr ? reinterpret_cast<MAddress>(target) : 0;
    ZPage* region = (from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader)
        ? Heap::page(from)
        : nullptr;
    const bool canLookup = from != 0 && Heap::IsHeapAddress(target) && verdict != HandVerdict::ZeroHeader;
    const MAddress lookupTo = canLookup
        ? forwarding_find(Heap::GetHeap().ObjectGeneration(target), from)
        : 0;
    // This is the last-chance diagnostic (zBarrier.inline.hpp:327-343). Pre-init callers, including
    // gc_unit other-vm children can enter before the generation cycle is active.
    const unsigned gcPhase = Heap::GetHeap().IsGcStarted() && ZGeneration::old() != nullptr
        ? static_cast<unsigned>(ZGeneration::old()->Snapshot().phase)
        : 0xffu;
    std::fprintf(stderr,
                 "[LOADFC][fail-closed] site=%s target=%p verdict=%u slotBits=%#zx "
                 "consumer=%s holder_kind=%s holder=%p slot=%p stage=%s writer_kind=%s "
                 "incoming_source_kind=%s source_slot=%p working_copy_slot=%p "
                 "field_type=%s field_offset=%zu from=%p from_region=%p "
                 "region_type=%u generation=%u in_current_relocation_set=%u "
                 "table_id=%#zx from_page_epoch=%llu lifeId=%llu "
                 "lookup_state=%u gc_phase=%u "
                 "unresolved non-Usable from-address must not be handed out\n",
                 site != nullptr ? site : "?", static_cast<void*>(target),
                 static_cast<unsigned>(verdict), slotBits,
                 site != nullptr ? site : "unknown",
                 ForwardingProvenance::KindName(provenance.kind),
                 provenance.holder, provenance.slot,
                 ForwardingProvenance::StageName(provenance.stage),
                 ForwardingProvenance::WriterName(provenance.writerKind),
                 ForwardingProvenance::SourceName(provenance.incomingSourceKind), provenance.sourceSlot,
                 provenance.workingCopySlot, ForwardingProvenance::FieldName(provenance.fieldKind),
                 provenance.fieldOffset, static_cast<void*>(target),
                 static_cast<void*>(region),
                 region != nullptr ? static_cast<unsigned>(0u) : 0xffu,
                 region != nullptr ? static_cast<unsigned>(region->generation_id()) : 0xffu,
                  lookupTo != 0 ? 1u : 0u,
                  static_cast<size_t>(0),
                  0ull,
                  0ull,
                  0u,
                 gcPhase);
    (void)fflush(stderr);
    (void)fflush(stdout);
    std::abort();
}

// Virtual default: this collector type does not implement the method. Always abort;
// body is out-of-line so HeapGcState.h stays free of FormatLog / string payloads.
}
