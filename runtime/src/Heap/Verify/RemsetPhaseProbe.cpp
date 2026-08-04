// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/RemsetPhaseProbe.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "Base/LogFile.h"

namespace MapleRuntime {
namespace RemsetPhaseProbe {
namespace {

constexpr size_t kPhaseBuckets = 16;
constexpr size_t kBarBuckets = 8;
constexpr size_t kReasonBuckets = 8;

struct SlotStamp {
    uint8_t phase = 0;
    uint8_t barClass = 0;
    uint8_t reason = REASON_UNKNOWN;
    uint8_t recorded = 0;
    uint32_t generation = 0;
};

std::mutex gLock;
std::unordered_map<MAddress, SlotStamp> gStamps;

std::array<std::atomic<uint64_t>, kPhaseBuckets> gWriteByPhase{};
std::array<std::atomic<uint64_t>, kPhaseBuckets> gRecordByPhase{};
std::array<std::atomic<uint64_t>, kPhaseBuckets> gMissingByPhase{};
std::array<std::atomic<uint64_t>, kBarBuckets> gWriteByBar{};
std::array<std::atomic<uint64_t>, kBarBuckets> gRecordByBar{};
std::array<std::atomic<uint64_t>, kBarBuckets> gMissingByBar{};
std::array<std::atomic<uint64_t>, kReasonBuckets> gWriteByReason{};
std::array<std::atomic<uint64_t>, kReasonBuckets> gMissingByReason{};
std::atomic<uint64_t> gMissingNoStamp{0};
std::atomic<uint64_t> gMissingTotal{0};
std::atomic<uint64_t> gWriteTotal{0};
std::atomic<uint64_t> gRecordTotal{0};

// Lifecycle counters (MRT_GCV2_REMSET_LIFECYCLE).
std::atomic<uint32_t> gStampGeneration{1};
std::atomic<uint64_t> gInsertTotal{0};
std::atomic<uint64_t> gEraseCalls{0};
std::atomic<uint64_t> gEraseSlots{0};
std::atomic<uint64_t> gAcquireCalls{0};
std::atomic<uint64_t> gAcquireSlotsSum{0};
std::atomic<uint64_t> gDrainCalls{0};
std::atomic<uint64_t> gDrainSlotsSum{0};
std::atomic<uint64_t> gMissingFreshRecorded{0};
std::atomic<uint64_t> gMissingStaleRecorded{0};
std::atomic<uint64_t> gMissingOtherStamp{0};
std::atomic<size_t> gLastRemsetSize{0};
std::atomic<uint64_t> gGenBumpCalls{0};

bool EnvOn(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

void Inc(std::array<std::atomic<uint64_t>, kPhaseBuckets>& arr, size_t idx)
{
    if (idx >= kPhaseBuckets) {
        idx = 0;
    }
    arr[idx].fetch_add(1, std::memory_order_relaxed);
}

void IncBar(std::array<std::atomic<uint64_t>, kBarBuckets>& arr, size_t idx)
{
    if (idx >= kBarBuckets) {
        idx = 0;
    }
    arr[idx].fetch_add(1, std::memory_order_relaxed);
}

void IncReason(std::array<std::atomic<uint64_t>, kReasonBuckets>& arr, size_t idx)
{
    if (idx >= kReasonBuckets) {
        idx = REASON_UNKNOWN;
    }
    arr[idx].fetch_add(1, std::memory_order_relaxed);
}

} // namespace

bool Enabled()
{
    static const bool on = EnvOn("MRT_GCV2_RECORD_REMSET_EVENTS") || EnvOn("MRT_GCPHASE_PROBE") ||
                           EnvOn("MRT_GCV2_REMSET_LIFECYCLE");
    return on;
}

bool LifecycleEnabled()
{
    static const bool on = EnvOn("MRT_GCV2_REMSET_LIFECYCLE");
    return on;
}

bool ForceRecordEnabled()
{
    static const bool on = EnvOn("MRT_GCPHASE_FORCE_RECORD");
    return on;
}

BarrierClass PhaseToBarrierClass(GCPhase phase)
{
    switch (phase) {
        case GCPhase::GC_PHASE_IDLE:
        case GCPhase::GC_PHASE_FINISH:
        case GCPhase::GC_PHASE_RECLAIM_SATB_NODE:
        case GCPhase::GC_PHASE_INIT:
        case GCPhase::GC_PHASE_UNDEF:
            return BAR_IDLE;
        case GCPhase::GC_PHASE_ENUM:
            return BAR_ENUM;
        case GCPhase::GC_PHASE_TRACE:
        case GCPhase::GC_PHASE_CLEAR_SATB_BUFFER:
            return BAR_TRACE;
        case GCPhase::GC_PHASE_POST_TRACE:
            return BAR_POST_TRACE;
        case GCPhase::GC_PHASE_PREFORWARD:
            return BAR_PREFORWARD;
        case GCPhase::GC_PHASE_FORWARD:
            return BAR_FORWARD;
        default:
            return BAR_OTHER;
    }
}

const char* PhaseName(GCPhase phase)
{
    switch (phase) {
        case GCPhase::GC_PHASE_UNDEF:
            return "UNDEF";
        case GCPhase::GC_PHASE_IDLE:
            return "IDLE";
        case GCPhase::GC_PHASE_FINISH:
            return "FINISH";
        case GCPhase::GC_PHASE_RECLAIM_SATB_NODE:
            return "RECLAIM_SATB";
        case GCPhase::GC_PHASE_INIT:
            return "INIT";
        case GCPhase::GC_PHASE_ENUM:
            return "ENUM";
        case GCPhase::GC_PHASE_TRACE:
            return "TRACE";
        case GCPhase::GC_PHASE_CLEAR_SATB_BUFFER:
            return "CLEAR_SATB";
        case GCPhase::GC_PHASE_POST_TRACE:
            return "POST_TRACE";
        case GCPhase::GC_PHASE_PREFORWARD:
            return "PREFORWARD";
        case GCPhase::GC_PHASE_FORWARD:
            return "FORWARD";
        default:
            return "OTHER";
    }
}

const char* BarrierClassName(BarrierClass bc)
{
    switch (bc) {
        case BAR_IDLE:
            return "Idle";
        case BAR_ENUM:
            return "Enum";
        case BAR_TRACE:
            return "Trace";
        case BAR_POST_TRACE:
            return "PostTrace";
        case BAR_PREFORWARD:
            return "Preforward";
        case BAR_FORWARD:
            return "Forward";
        case BAR_OTHER:
            return "Other";
        default:
            return "Undef";
    }
}

const char* SkipReasonName(SkipReason r)
{
    switch (r) {
        case REASON_RECORDED:
            return "recorded";
        case REASON_NO_YOUNG:
            return "no_young";
        case REASON_REF_NULL_OR_NONHEAP:
            return "ref_null_or_nonheap";
        case REASON_REF_NOT_YOUNG:
            return "ref_not_young";
        case REASON_HOLDER_NULL_OR_NONHEAP:
            return "holder_null_or_nonheap";
        case REASON_HOLDER_YOUNG:
            return "holder_young";
        case REASON_NO_STAMP:
            return "no_barrier_record";
        default:
            return "unknown";
    }
}

void NoteWrite(MAddress fieldAddress, GCPhase phase, SkipReason reason, bool recorded)
{
    if (!Enabled()) {
        return;
    }
    size_t p = static_cast<size_t>(phase);
    BarrierClass bc = PhaseToBarrierClass(phase);
    gWriteTotal.fetch_add(1, std::memory_order_relaxed);
    Inc(gWriteByPhase, p);
    IncBar(gWriteByBar, static_cast<size_t>(bc));
    IncReason(gWriteByReason, static_cast<size_t>(reason));
    if (recorded) {
        gRecordTotal.fetch_add(1, std::memory_order_relaxed);
        Inc(gRecordByPhase, p);
        IncBar(gRecordByBar, static_cast<size_t>(bc));
    }
    SlotStamp st;
    st.phase = static_cast<uint8_t>(phase);
    st.barClass = static_cast<uint8_t>(bc);
    st.reason = static_cast<uint8_t>(reason);
    st.recorded = recorded ? 1 : 0;
    st.generation = gStampGeneration.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(gLock);
    gStamps[fieldAddress] = st;
}

void NoteMissing(MAddress fieldAddress)
{
    if (!Enabled()) {
        return;
    }
    gMissingTotal.fetch_add(1, std::memory_order_relaxed);
    SlotStamp st;
    bool found = false;
    uint32_t curGen = gStampGeneration.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto it = gStamps.find(fieldAddress);
        if (it != gStamps.end()) {
            st = it->second;
            found = true;
        }
    }
    if (!found) {
        gMissingNoStamp.fetch_add(1, std::memory_order_relaxed);
        IncReason(gMissingByReason, REASON_NO_STAMP);
        // Count under UNDEF phase / Undef bar for distribution tables.
        Inc(gMissingByPhase, 0);
        IncBar(gMissingByBar, BAR_UNDEF);
        VLOG(REPORT,
             "[GCV2][remset][recorded][MISSING_EVENT] slot=%p event=no_barrier_record "
             "env=MRT_GCV2_RECORD_REMSET_EVENTS=1",
             reinterpret_cast<void*>(fieldAddress));
        return;
    }
    Inc(gMissingByPhase, st.phase);
    IncBar(gMissingByBar, st.barClass);
    IncReason(gMissingByReason, st.reason);
    const char* freshness = "n/a";
    if (LifecycleEnabled() && st.recorded != 0) {
        if (st.generation == curGen) {
            gMissingFreshRecorded.fetch_add(1, std::memory_order_relaxed);
            freshness = "fresh";
        } else {
            gMissingStaleRecorded.fetch_add(1, std::memory_order_relaxed);
            freshness = "stale";
        }
    } else if (LifecycleEnabled()) {
        gMissingOtherStamp.fetch_add(1, std::memory_order_relaxed);
        freshness = "other";
    }
    VLOG(REPORT,
         "[GCV2][remset][recorded][MISSING_EVENT] slot=%p phase=%s(%u) barrier=%s reason=%s recorded=%u "
         "stampGen=%u curGen=%u freshness=%s env=MRT_GCV2_RECORD_REMSET_EVENTS=1",
         reinterpret_cast<void*>(fieldAddress), PhaseName(static_cast<GCPhase>(st.phase)),
         static_cast<unsigned int>(st.phase), BarrierClassName(static_cast<BarrierClass>(st.barClass)),
         SkipReasonName(static_cast<SkipReason>(st.reason)), static_cast<unsigned int>(st.recorded),
         static_cast<unsigned int>(st.generation), static_cast<unsigned int>(curGen), freshness);
}

void ClearSlotStamps()
{
    if (!Enabled()) {
        return;
    }
    std::lock_guard<std::mutex> guard(gLock);
    gStamps.clear();
}

void NoteRemsetInsert(MAddress fieldAddress, size_t sizeAfter)
{
    if (!LifecycleEnabled()) {
        return;
    }
    (void)fieldAddress;
    gInsertTotal.fetch_add(1, std::memory_order_relaxed);
    gLastRemsetSize.store(sizeAfter, std::memory_order_relaxed);
}

void NoteRemsetEraseRange(MAddress start, MAddress end, size_t erased, size_t scanned, const char* site)
{
    if (!LifecycleEnabled()) {
        return;
    }
    gEraseCalls.fetch_add(1, std::memory_order_relaxed);
    gEraseSlots.fetch_add(erased, std::memory_order_relaxed);
    if (erased != 0) {
        VLOG(REPORT,
             "[GCV2][remset][lifecycle][ERASE] site=%s start=%p end=%p erased=%zu scanned=%zu gen=%u "
             "env=MRT_GCV2_REMSET_LIFECYCLE=1",
             site == nullptr ? "?" : site, reinterpret_cast<void*>(start), reinterpret_cast<void*>(end), erased,
             scanned, static_cast<unsigned int>(gStampGeneration.load(std::memory_order_relaxed)));
    }
}

void NoteRemsetAcquire(size_t sizeBeforeClear, const char* site)
{
    if (!LifecycleEnabled()) {
        return;
    }
    gAcquireCalls.fetch_add(1, std::memory_order_relaxed);
    gAcquireSlotsSum.fetch_add(sizeBeforeClear, std::memory_order_relaxed);
    gLastRemsetSize.store(0, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][remset][lifecycle][ACQUIRE] site=%s size=%zu gen=%u inserts=%llu erases=%llu erasedSlots=%llu "
         "env=MRT_GCV2_REMSET_LIFECYCLE=1",
         site == nullptr ? "?" : site, sizeBeforeClear,
         static_cast<unsigned int>(gStampGeneration.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gInsertTotal.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gEraseCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gEraseSlots.load(std::memory_order_relaxed)));
}

void NoteRemsetDrain(size_t dropped, const char* site)
{
    if (!LifecycleEnabled()) {
        return;
    }
    gDrainCalls.fetch_add(1, std::memory_order_relaxed);
    gDrainSlotsSum.fetch_add(dropped, std::memory_order_relaxed);
    gLastRemsetSize.store(0, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][remset][lifecycle][DRAIN] site=%s dropped=%zu gen=%u env=MRT_GCV2_REMSET_LIFECYCLE=1",
         site == nullptr ? "?" : site, dropped,
         static_cast<unsigned int>(gStampGeneration.load(std::memory_order_relaxed)));
}

void BumpStampGeneration(const char* site)
{
    if (!LifecycleEnabled() && !Enabled()) {
        return;
    }
    if (!LifecycleEnabled()) {
        return;
    }
    uint32_t next = gStampGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    gGenBumpCalls.fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT, "[GCV2][remset][lifecycle][GEN_BUMP] site=%s gen=%u env=MRT_GCV2_REMSET_LIFECYCLE=1",
         site == nullptr ? "?" : site, static_cast<unsigned int>(next));
}

void DumpLifecycle(const char* tag)
{
    if (!LifecycleEnabled()) {
        return;
    }
    VLOG(REPORT,
         "[GCV2][remset][lifecycle][SUMMARY] tag=%s gen=%u inserts=%llu eraseCalls=%llu erasedSlots=%llu "
         "acquireCalls=%llu acquireSlotsSum=%llu drainCalls=%llu drainSlotsSum=%llu "
         "missingFreshRecorded=%llu missingStaleRecorded=%llu missingOtherStamp=%llu "
         "missingNoStamp=%llu missingTotal=%llu recordTotal=%llu lastSize=%zu "
         "env=MRT_GCV2_REMSET_LIFECYCLE=1",
         tag == nullptr ? "?" : tag, static_cast<unsigned int>(gStampGeneration.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gInsertTotal.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gEraseCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gEraseSlots.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gAcquireCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gAcquireSlotsSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gDrainCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gDrainSlotsSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMissingFreshRecorded.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMissingStaleRecorded.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMissingOtherStamp.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMissingNoStamp.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gMissingTotal.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(gRecordTotal.load(std::memory_order_relaxed)),
         gLastRemsetSize.load(std::memory_order_relaxed));
}

void DumpSummary(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    uint64_t writeTotal = gWriteTotal.load(std::memory_order_relaxed);
    uint64_t recordTotal = gRecordTotal.load(std::memory_order_relaxed);
    uint64_t missingTotal = gMissingTotal.load(std::memory_order_relaxed);
    uint64_t missingNoStamp = gMissingNoStamp.load(std::memory_order_relaxed);

    VLOG(REPORT,
         "[GCPHASE] tag=%s writeTotal=%llu recordTotal=%llu missingTotal=%llu missingNoStamp=%llu "
         "(no_barrier_record)",
         tag == nullptr ? "?" : tag, static_cast<unsigned long long>(writeTotal),
         static_cast<unsigned long long>(recordTotal), static_cast<unsigned long long>(missingTotal),
         static_cast<unsigned long long>(missingNoStamp));

    // MISSING_BY_PHASE
    for (size_t i = 0; i < kPhaseBuckets; ++i) {
        uint64_t m = gMissingByPhase[i].load(std::memory_order_relaxed);
        uint64_t w = gWriteByPhase[i].load(std::memory_order_relaxed);
        uint64_t r = gRecordByPhase[i].load(std::memory_order_relaxed);
        if (m == 0 && w == 0 && r == 0) {
            continue;
        }
        double pct = missingTotal == 0 ? 0.0 : (100.0 * static_cast<double>(m) / static_cast<double>(missingTotal));
        VLOG(REPORT, "[GCPHASE][MISSING_BY_PHASE] phase=%s(%zu) missing=%llu (%.1f%%) write=%llu record=%llu",
             PhaseName(static_cast<GCPhase>(i)), i, static_cast<unsigned long long>(m), pct,
             static_cast<unsigned long long>(w), static_cast<unsigned long long>(r));
    }

    // MISSING_BY_BARRIER_CLASS
    for (size_t i = 0; i < kBarBuckets; ++i) {
        uint64_t m = gMissingByBar[i].load(std::memory_order_relaxed);
        uint64_t w = gWriteByBar[i].load(std::memory_order_relaxed);
        uint64_t r = gRecordByBar[i].load(std::memory_order_relaxed);
        if (m == 0 && w == 0 && r == 0) {
            continue;
        }
        double pct = missingTotal == 0 ? 0.0 : (100.0 * static_cast<double>(m) / static_cast<double>(missingTotal));
        VLOG(REPORT, "[GCPHASE][MISSING_BY_BARRIER_CLASS] bar=%s missing=%llu (%.1f%%) write=%llu record=%llu",
             BarrierClassName(static_cast<BarrierClass>(i)), static_cast<unsigned long long>(m), pct,
             static_cast<unsigned long long>(w), static_cast<unsigned long long>(r));
    }

    // MISSING_BY_REASON (incl. no_barrier_record)
    for (size_t i = 0; i < kReasonBuckets; ++i) {
        uint64_t m = gMissingByReason[i].load(std::memory_order_relaxed);
        uint64_t w = gWriteByReason[i].load(std::memory_order_relaxed);
        if (m == 0 && w == 0) {
            continue;
        }
        double pct = missingTotal == 0 ? 0.0 : (100.0 * static_cast<double>(m) / static_cast<double>(missingTotal));
        VLOG(REPORT, "[GCPHASE][MISSING_BY_REASON] reason=%s missing=%llu (%.1f%%) write_skip=%llu",
             SkipReasonName(static_cast<SkipReason>(i)), static_cast<unsigned long long>(m), pct,
             static_cast<unsigned long long>(w));
    }

    DumpLifecycle(tag);
}

} // namespace RemsetPhaseProbe
} // namespace MapleRuntime
