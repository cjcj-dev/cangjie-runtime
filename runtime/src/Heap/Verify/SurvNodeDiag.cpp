// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/SurvNodeDiag.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/MarkCompleteVerify.h"

namespace MapleRuntime {
namespace SurvNodeDiag {
namespace {

constexpr size_t kWriteSlots = 1u << 21;
constexpr size_t kWriteMask = kWriteSlots - 1;
constexpr size_t kPaintSlots = 1u << 18;
constexpr size_t kPaintMask = kPaintSlots - 1;
constexpr size_t kFollowSlots = 1u << 20;
constexpr size_t kFollowMask = kFollowSlots - 1;
constexpr size_t kClearRing = 256;

struct WriteRec {
    std::atomic<uintptr_t> slot{ 0 };
    std::atomic<uintptr_t> neu{ 0 };
    std::atomic<uintptr_t> pre{ 0 };
    std::atomic<uint32_t> gcCount{ 0 };
    std::atomic<uint8_t> phase{ 0 };
    std::atomic<uint8_t> site{ 0 };
};

struct PaintRec {
    std::atomic<uintptr_t> obj{ 0 };
    std::atomic<uintptr_t> region{ 0 };
    std::atomic<uintptr_t> liveInfo{ 0 };
    std::atomic<uint32_t> gcCount{ 0 };
    std::atomic<uint64_t> epoch{ 0 };
};

struct ClearRec {
    uintptr_t region = 0;
    uintptr_t liveInfoBefore = 0;
    uint64_t epochBefore = 0;
    uint64_t epochAfter = 0;
    uint32_t gcCount = 0;
    uint8_t phase = 0;
    uint8_t site = 0;
    uint8_t epochBumped = 0;
    uint8_t genOld = 0;
};

struct FollowRec {
    std::atomic<uintptr_t> holder{ 0 };
    std::atomic<uint32_t> gcCount{ 0 };
    std::atomic<uint8_t> action{ 0 };
};

WriteRec g_writes[kWriteSlots];
WriteRec g_visits[kWriteSlots];
PaintRec g_paints[kPaintSlots];
FollowRec g_follows[kFollowSlots];
ClearRec g_clears[kClearRing];
std::atomic<uint64_t> g_clearHead{ 0 };

std::atomic<uint64_t> g_storeSeen{ 0 };
std::atomic<uint64_t> g_storeFromUnmarked{ 0 };
std::atomic<uint64_t> g_storeDuringTrace{ 0 };
std::atomic<uint64_t> g_paintSeen{ 0 };
std::atomic<uint64_t> g_clearSeen{ 0 };
std::atomic<uint64_t> g_deadLookupHit{ 0 };
std::atomic<uint64_t> g_deadLookupMiss{ 0 };
std::atomic<uint64_t> g_deadPaintedThenClear{ 0 };
std::atomic<uint64_t> g_deadNeverPainted{ 0 };
std::atomic<uint64_t> g_deadWriteAfterTrace{ 0 };
std::atomic<uint64_t> g_deadWriteDuringTrace{ 0 };
std::atomic<uint64_t> g_deadWriteIdle{ 0 };
std::atomic<uint64_t> g_tracePush{ 0 };
std::atomic<uint64_t> g_traceSkipMarked{ 0 };
std::atomic<uint64_t> g_traceSkipGate{ 0 };
std::atomic<uint64_t> g_deadVisitHit{ 0 };
std::atomic<uint64_t> g_deadVisitMiss{ 0 };
std::atomic<uint64_t> g_deadVisitPush{ 0 };
std::atomic<uint64_t> g_deadVisitSkipMarked{ 0 };
std::atomic<uint64_t> g_followScan{ 0 };
std::atomic<uint64_t> g_followSkipMarked{ 0 };
std::atomic<uint64_t> g_followSkipGate{ 0 };
std::atomic<uint64_t> g_deadFollowScan{ 0 };
std::atomic<uint64_t> g_deadFollowSkipMarked{ 0 };
std::atomic<uint64_t> g_deadFollowMiss{ 0 };
std::atomic<uint64_t> g_deadRemsetHit{ 0 };

size_t WriteIndex(uintptr_t slot) { return static_cast<size_t>((slot >> 3) & kWriteMask); }
size_t PaintIndex(uintptr_t obj) { return static_cast<size_t>((obj >> 4) & kPaintMask); }
size_t FollowIndex(uintptr_t holder) { return static_cast<size_t>((holder >> 4) & kFollowMask); }

const char* SiteName(uint8_t site)
{
    switch (site) {
        case STORE_WRITE_REF: return "WriteReference";
        case STORE_ATOMIC_WRITE: return "AtomicWrite";
        case STORE_CAS: return "CAS";
        case STORE_SWAP: return "Swap";
        case STORE_COPY_REF: return "CopyRefArray";
        case STORE_WRITE_STATIC: return "WriteStatic";
        case 0x41: return "TracePush";
        case 0x42: return "TraceSkipMarked";
        case 0x43: return "TraceSkipGate";
        case 0x44: return "TraceSkipStale";
        default: return "?";
    }
}

const char* FollowName(uint8_t action)
{
    switch (action) {
        case FOLLOW_SCAN: return "scan";
        case FOLLOW_SKIP_MARKED: return "skipMarked";
        case FOLLOW_SKIP_GATE: return "skipGate";
        default: return "-";
    }
}

const char* ClearName(uint8_t site)
{
    switch (site) {
        case CLEAR_LIVE_INFO: return "ClearLiveInfo";
        case CLEAR_CHECK_AND_CLEAR: return "CheckAndClearLiveInfo";
        case CLEAR_NULL_IN_RANGE: return "NullLiveInfoFieldsInRange";
        case CLEAR_RESET_MARK_BIT: return "ResetMarkBit";
        default: return "?";
    }
}

bool PhaseAfterTrace(uint8_t phase)
{
    return phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD || phase == GC_PHASE_FORWARD ||
        phase == GC_PHASE_FINISH || phase == GC_PHASE_RECLAIM_SATB_NODE;
}

} // namespace

bool Enabled() { return MarkCompleteVerify::Enabled(); }

void NoteStore(const void* slot, BaseObject* pre, BaseObject* neu, uint8_t site)
{
    if (!Enabled() || slot == nullptr) {
        return;
    }
    if (neu == nullptr || !Heap::IsHeapAddress(neu)) {
        return;
    }
    if (RegionSpace::IsMarkedObject<Generation::Old>(neu)) {
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(neu));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return;
    }
    const uint8_t phase = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
    const bool fromSpace =
        region->IsFromRegion() || region->IsLoneFromRegion() || region->IsUnmovableFromRegion();
    const bool duringTrace =
        phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER || phase == GC_PHASE_ENUM;
    // LEAD-NOTE U1: TRACE-window stores of unmarked heap targets, not only from-space.
    if (!fromSpace && !duringTrace) {
        return;
    }
    const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    WriteRec& rec = g_writes[WriteIndex(slotAddr)];
    rec.neu.store(reinterpret_cast<uintptr_t>(neu), std::memory_order_relaxed);
    rec.pre.store(reinterpret_cast<uintptr_t>(pre), std::memory_order_relaxed);
    rec.gcCount.store(static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    rec.phase.store(phase, std::memory_order_relaxed);
    rec.site.store(site, std::memory_order_relaxed);
    rec.slot.store(slotAddr, std::memory_order_release);
    g_storeSeen.fetch_add(1, std::memory_order_relaxed);
    if (fromSpace) {
        g_storeFromUnmarked.fetch_add(1, std::memory_order_relaxed);
    }
    if (duringTrace) {
        g_storeDuringTrace.fetch_add(1, std::memory_order_relaxed);
    }
}

void NotePaint(BaseObject* obj, RegionInfo* region)
{
    if (!Enabled() || obj == nullptr || region == nullptr) {
        return;
    }
    if (!(region->IsFromRegion() || region->IsLoneFromRegion() || region->IsUnmovableFromRegion())) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    PaintRec& rec = g_paints[PaintIndex(addr)];
    rec.region.store(reinterpret_cast<uintptr_t>(region), std::memory_order_relaxed);
    LiveInfo* live = region == nullptr ? nullptr : region->GetLiveInfo();
    rec.liveInfo.store(reinterpret_cast<uintptr_t>(live), std::memory_order_relaxed);
    rec.gcCount.store(static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    rec.epoch.store(region == nullptr ? 0ULL : region->GetMarkSnapshotEpoch<Generation::Old>(),
                    std::memory_order_relaxed);
    rec.obj.store(addr, std::memory_order_release);
    g_paintSeen.fetch_add(1, std::memory_order_relaxed);
}

void NoteTraceVisit(const void* slot, BaseObject* target, uint8_t action)
{
    if (!Enabled() || slot == nullptr || target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (region == nullptr ||
        !(region->IsFromRegion() || region->IsLoneFromRegion() || region->IsUnmovableFromRegion())) {
        return;
    }
    const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    WriteRec& rec = g_visits[WriteIndex(slotAddr)];
    rec.neu.store(reinterpret_cast<uintptr_t>(target), std::memory_order_relaxed);
    rec.pre.store(0, std::memory_order_relaxed);
    rec.gcCount.store(static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    rec.phase.store(static_cast<uint8_t>(Heap::GetHeap().GetGCPhase()), std::memory_order_relaxed);
    rec.site.store(static_cast<uint8_t>(0x40u | action), std::memory_order_relaxed);
    rec.slot.store(slotAddr, std::memory_order_release);
    if (action == TRACE_PUSH) {
        g_tracePush.fetch_add(1, std::memory_order_relaxed);
    } else if (action == TRACE_SKIP_MARKED) {
        g_traceSkipMarked.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_traceSkipGate.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteFollowHolder(BaseObject* holder, uint8_t action)
{
    if (!Enabled() || holder == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(holder);
    FollowRec& rec = g_follows[FollowIndex(addr)];
    if (rec.holder.load(std::memory_order_acquire) == addr &&
        rec.action.load(std::memory_order_relaxed) == FOLLOW_SKIP_GATE && action == FOLLOW_SKIP_MARKED) {
        return;
    }
    rec.gcCount.store(static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    rec.action.store(action, std::memory_order_relaxed);
    rec.holder.store(addr, std::memory_order_release);
    if (action == FOLLOW_SCAN) {
        g_followScan.fetch_add(1, std::memory_order_relaxed);
    } else if (action == FOLLOW_SKIP_MARKED) {
        g_followSkipMarked.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_followSkipGate.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteClear(RegionInfo* region, uint8_t site, bool epochBumped)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    const uint64_t n = g_clearHead.fetch_add(1, std::memory_order_relaxed);
    ClearRec& rec = g_clears[n & (kClearRing - 1)];
    rec.region = reinterpret_cast<uintptr_t>(region);
    LiveInfo* live = region->GetLiveInfo();
    rec.liveInfoBefore = reinterpret_cast<uintptr_t>(live);
    rec.epochBefore = region->GetMarkSnapshotEpoch<Generation::Old>();
    rec.gcCount = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    rec.phase = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
    rec.site = site;
    rec.epochBumped = epochBumped ? 1u : 0u;
    rec.genOld = 1u;
    rec.epochAfter = rec.epochBefore; // caller has not bumped yet; Report recomputes
    g_clearSeen.fetch_add(1, std::memory_order_relaxed);
    const uint64_t seen = n + 1;
    if (seen <= 4 || (seen & (seen - 1)) == 0) {
        LOG(RTLOG_ERROR,
            "[GCV2][survnode] CLEAR n=%llu region=%p site=%s phase=%s gc=%u epoch=%llu liveInfo=%p bumped=%u",
            static_cast<unsigned long long>(seen), region, ClearName(site),
            Collector::GetGCPhaseName(static_cast<GCPhase>(rec.phase)), rec.gcCount,
            static_cast<unsigned long long>(rec.epochBefore), live, static_cast<unsigned>(rec.epochBumped));
    }
}

void ReportOnDeadEdge(BaseObject* holder, void* slot, BaseObject* target, RegionInfo* targetRegion)
{
    if (!Enabled()) {
        return;
    }
    const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    WriteRec& wrec = g_writes[WriteIndex(slotAddr)];
    const uintptr_t recSlot = wrec.slot.load(std::memory_order_acquire);
    const bool writeHit = recSlot == slotAddr;
    uint8_t phase = 0;
    uint8_t site = 0;
    uint32_t writeGc = 0;
    uintptr_t neu = 0;
    uintptr_t pre = 0;
    if (writeHit) {
        g_deadLookupHit.fetch_add(1, std::memory_order_relaxed);
        phase = wrec.phase.load(std::memory_order_relaxed);
        site = wrec.site.load(std::memory_order_relaxed);
        writeGc = wrec.gcCount.load(std::memory_order_relaxed);
        neu = wrec.neu.load(std::memory_order_relaxed);
        pre = wrec.pre.load(std::memory_order_relaxed);
        if (PhaseAfterTrace(phase)) {
            g_deadWriteAfterTrace.fetch_add(1, std::memory_order_relaxed);
        } else if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER || phase == GC_PHASE_ENUM) {
            g_deadWriteDuringTrace.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_deadWriteIdle.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_deadLookupMiss.fetch_add(1, std::memory_order_relaxed);
    }

    WriteRec& vrec = g_visits[WriteIndex(slotAddr)];
    const bool visitHit = vrec.slot.load(std::memory_order_acquire) == slotAddr;
    uint8_t visitSite = 0;
    uintptr_t visitNeu = 0;
    if (visitHit) {
        g_deadVisitHit.fetch_add(1, std::memory_order_relaxed);
        visitSite = vrec.site.load(std::memory_order_relaxed);
        visitNeu = vrec.neu.load(std::memory_order_relaxed);
        if (visitSite == 0x41) {
            g_deadVisitPush.fetch_add(1, std::memory_order_relaxed);
        } else if (visitSite == 0x42) {
            g_deadVisitSkipMarked.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_deadVisitMiss.fetch_add(1, std::memory_order_relaxed);
    }

    const uintptr_t holderAddr = reinterpret_cast<uintptr_t>(holder);
    FollowRec& frec = g_follows[FollowIndex(holderAddr)];
    const bool followHit = frec.holder.load(std::memory_order_acquire) == holderAddr;
    uint8_t followAction = 0;
    if (followHit) {
        followAction = frec.action.load(std::memory_order_relaxed);
        if (followAction == FOLLOW_SCAN) {
            g_deadFollowScan.fetch_add(1, std::memory_order_relaxed);
        } else if (followAction == FOLLOW_SKIP_MARKED) {
            g_deadFollowSkipMarked.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_deadFollowMiss.fetch_add(1, std::memory_order_relaxed);
    }

    const unsigned remsetHit =
        slot == nullptr ? 0u : (Heap::GetHeap().GetRememberedSet().Contains(slotAddr) ? 1u : 0u);
    if (remsetHit != 0) {
        g_deadRemsetHit.fetch_add(1, std::memory_order_relaxed);
    }

    const uintptr_t tgtAddr = reinterpret_cast<uintptr_t>(target);
    PaintRec& prec = g_paints[PaintIndex(tgtAddr)];
    const bool paintHit = prec.obj.load(std::memory_order_acquire) == tgtAddr;
    const uint32_t nowGc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    const bool paintedThisCycle = paintHit && prec.gcCount.load(std::memory_order_relaxed) == nowGc;
    if (paintedThisCycle) {
        g_deadPaintedThenClear.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_deadNeverPainted.fetch_add(1, std::memory_order_relaxed);
    }

    uintptr_t lastClearRegion = 0;
    uint8_t lastClearSite = 0;
    uint8_t lastClearPhase = 0;
    uint8_t lastClearBumped = 0;
    uint32_t lastClearGc = 0;
    uint64_t lastClearEpoch = 0;
    if (targetRegion != nullptr) {
        const uintptr_t want = reinterpret_cast<uintptr_t>(targetRegion);
        const uint64_t head = g_clearHead.load(std::memory_order_relaxed);
        const uint64_t begin = head > kClearRing ? head - kClearRing : 0;
        for (uint64_t i = head; i > begin; --i) {
            const ClearRec& c = g_clears[(i - 1) & (kClearRing - 1)];
            if (c.region == want) {
                lastClearRegion = c.region;
                lastClearSite = c.site;
                lastClearPhase = c.phase;
                lastClearBumped = c.epochBumped;
                lastClearGc = c.gcCount;
                lastClearEpoch = c.epochBefore;
                break;
            }
        }
    }

    LiveInfo* liveNow = targetRegion == nullptr ? nullptr : targetRegion->GetLiveInfo();
    const unsigned markedNow =
        target == nullptr ? 0u : (RegionSpace::IsMarkedObject<Generation::Old>(target) ? 1u : 0u);

    LOG(RTLOG_ERROR,
        "[GCV2][survnode] DEAD_SLOT holder=%p slot=%p target=%p "
        "writeHit=%u writePhase=%s writeSite=%s writeGc=%u neu=%p pre=%p "
        "visitHit=%u visitSite=%s visitNeu=%p visitSame=%u followHit=%u follow=%s remset=%u "
        "paintHit=%u paintedThisCycle=%u paintGc=%u paintLiveInfo=%p paintEpoch=%llu "
        "liveInfoNow=%p markedNow=%u "
        "clearHit=%u clearSite=%s clearPhase=%s clearGc=%u clearEpoch=%llu clearBumped=%u",
        holder, slot, target, static_cast<unsigned>(writeHit),
        writeHit ? Collector::GetGCPhaseName(static_cast<GCPhase>(phase)) : "-", writeHit ? SiteName(site) : "-",
        writeGc, reinterpret_cast<void*>(neu), reinterpret_cast<void*>(pre), static_cast<unsigned>(visitHit),
        visitHit ? SiteName(visitSite) : "-", reinterpret_cast<void*>(visitNeu),
        static_cast<unsigned>(visitHit && visitNeu == reinterpret_cast<uintptr_t>(target)),
        static_cast<unsigned>(followHit),
        followHit ? FollowName(followAction) : "-", remsetHit, static_cast<unsigned>(paintHit),
        static_cast<unsigned>(paintedThisCycle), paintHit ? prec.gcCount.load(std::memory_order_relaxed) : 0u,
        paintHit ? reinterpret_cast<void*>(prec.liveInfo.load(std::memory_order_relaxed)) : nullptr,
        paintHit ? static_cast<unsigned long long>(prec.epoch.load(std::memory_order_relaxed)) : 0ULL, liveNow,
        markedNow, static_cast<unsigned>(lastClearRegion != 0), lastClearRegion != 0 ? ClearName(lastClearSite) : "-",
        lastClearRegion != 0 ? Collector::GetGCPhaseName(static_cast<GCPhase>(lastClearPhase)) : "-", lastClearGc,
        static_cast<unsigned long long>(lastClearEpoch), static_cast<unsigned>(lastClearBumped));
}

void ReportAtMarkEnd(const char* point)
{
    if (!Enabled()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][survnode] point=%s storeSeen=%llu storeFromUnmarked=%llu storeDuringTrace=%llu "
        "paintSeen=%llu clearSeen=%llu "
        "tracePush=%llu traceSkipMarked=%llu traceSkipGate=%llu "
        "followScan=%llu followSkipMarked=%llu followSkipGate=%llu "
        "deadWriteHit=%llu deadWriteMiss=%llu deadWriteAfterTrace=%llu deadWriteDuringTrace=%llu "
        "deadWriteIdle=%llu deadVisitHit=%llu deadVisitMiss=%llu deadVisitPush=%llu deadVisitSkipMarked=%llu "
        "deadFollowScan=%llu deadFollowSkipMarked=%llu deadFollowMiss=%llu deadRemsetHit=%llu "
        "deadPaintedThenClear=%llu deadNeverPainted=%llu",
        point == nullptr ? "?" : point, static_cast<unsigned long long>(g_storeSeen.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_storeFromUnmarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_storeDuringTrace.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_paintSeen.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_clearSeen.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_tracePush.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_traceSkipMarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_traceSkipGate.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_followScan.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_followSkipMarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_followSkipGate.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadLookupHit.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadLookupMiss.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadWriteAfterTrace.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadWriteDuringTrace.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadWriteIdle.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadVisitHit.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadVisitMiss.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadVisitPush.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadVisitSkipMarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadFollowScan.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadFollowSkipMarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadFollowMiss.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadRemsetHit.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadPaintedThenClear.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_deadNeverPainted.load(std::memory_order_relaxed)));
}

} // namespace SurvNodeDiag
} // namespace MapleRuntime
