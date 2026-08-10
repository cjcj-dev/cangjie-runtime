// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/TipWhoDiag.h"

#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/ColourTypes.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace TipWhoDiag {
namespace {

std::atomic<int> g_enabled{ -1 };
std::atomic<size_t> g_sampleN{ 0 };
std::atomic<size_t> g_softN{ 0 };
std::atomic<size_t> g_visitGateN{ 0 };
std::atomic<size_t> g_prePubN{ 0 };
std::atomic<size_t> g_permN{ 0 };
std::atomic<size_t> g_uncopiedStarts{ 0 };
std::atomic<size_t> g_interiorMarks{ 0 };
std::atomic<size_t> g_orphanMarks{ 0 };
std::atomic<size_t> g_paintN{ 0 };

// Ring of recent MarkObject paints for orphan-bit attribution.
struct PaintRec {
    uintptr_t region;
    uintptr_t rstart;
    uintptr_t obj;
    size_t offset;
    size_t objSize;
    size_t gcCount;
    unsigned phase;
    void* ra0;
    void* ra1;
    char site[40];
};
constexpr size_t kPaintRing = 4096;
PaintRec g_paintRing[kPaintRing];
std::atomic<size_t> g_paintSeq{ 0 };

size_t MaxSamples()
{
    static size_t cap = []() {
        const char* v = std::getenv("MRT_GCV2_TIPWHO_MAX");
        if (v == nullptr || v[0] == '\0') {
            return static_cast<size_t>(64);
        }
        char* end = nullptr;
        unsigned long n = std::strtoul(v, &end, 10);
        if (end == v || n == 0) {
            return static_cast<size_t>(64);
        }
        return static_cast<size_t>(n);
    }();
    return cap;
}

bool SampleOk()
{
    size_t n = g_sampleN.fetch_add(1, std::memory_order_relaxed) + 1;
    return n <= MaxSamples();
}

// Classify offset relative to size-walk of region payload.
// kind: "start" | "interior" | "orphan" | "past_alloc" | "walk_break"
// hostStart: for interior, the size-walk start that covers offset; else 0
// hostSz: size of that host; else 0
void ClassifyOffset(RegionInfo* region, size_t offset, const char*& kind, size_t& hostStart, size_t& hostSz,
                    size_t& walkStarts, size_t& walkBreakOff)
{
    kind = "past_alloc";
    hostStart = 0;
    hostSz = 0;
    walkStarts = 0;
    walkBreakOff = 0;
    if (region == nullptr) {
        kind = "no_region";
        return;
    }
    uintptr_t rstart = region->GetRegionStart();
    uintptr_t alloc = region->GetRegionAllocPtr();
    if (offset >= static_cast<size_t>(alloc - rstart)) {
        kind = "past_alloc";
        return;
    }
    uintptr_t position = rstart;
    size_t curOff = 0;
    while (position < alloc) {
        BaseObject* obj = from_region_addr(position);
        if (!obj->IsValidObject()) {
            walkBreakOff = curOff;
            kind = (curOff == offset) ? "walk_break" : (curOff < offset ? "orphan" : kind);
            if (curOff < offset) {
                kind = "orphan";
            }
            return;
        }
        size_t sz = 0;
        // Prefer GetAllocSize; if tip looks bad, stop (same hazard as VisitLive).
        TypeInfo* tip = obj->GetTypeInfo();
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
        if (tip == nullptr || tipAddr < 0x1000U || (tipAddr & 0x7U) != 0 || Heap::IsHeapAddress(tipAddr)) {
            walkBreakOff = curOff;
            if (curOff == offset) {
                kind = "walk_break";
            } else if (curOff < offset) {
                kind = "orphan";
            }
            return;
        }
        sz = RegionSpace::GetAllocSize(*obj);
        if (sz == 0) {
            walkBreakOff = curOff;
            kind = (curOff <= offset) ? "walk_break" : kind;
            return;
        }
        if (region->IsSurvivedObject(curOff)) {
            ++walkStarts;
        }
        if (curOff == offset) {
            kind = "start";
            hostStart = curOff;
            hostSz = sz;
            return;
        }
        if (curOff < offset && offset < curOff + sz) {
            kind = "interior";
            hostStart = curOff;
            hostSz = sz;
            return;
        }
        if (curOff > offset) {
            kind = "orphan";
            return;
        }
        position += sz;
        curOff += sz;
    }
    kind = "orphan";
}

} // namespace

bool Enabled()
{
    int e = g_enabled.load(std::memory_order_relaxed);
    if (e >= 0) {
        return e != 0;
    }
    const char* v = std::getenv("MRT_GCV2_TIPWHO");
    int on = (v != nullptr && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    g_enabled.store(on, std::memory_order_relaxed);
    return on != 0;
}

void NoteSoftReturn(BaseObject* obj, RegionInfo* region, const char* branch, BaseObject* toObj)
{
    if (!Enabled()) {
        return;
    }
    g_softN.fetch_add(1, std::memory_order_relaxed);
    if (!SampleOk()) {
        return;
    }
    size_t offset = 0;
    size_t rstart = 0;
    size_t allocOff = 0;
    unsigned rs = 0;
    unsigned young = 0;
    unsigned survived = 0;
    unsigned objFwd = 0;
    size_t live = 0;
    if (region != nullptr && obj != nullptr) {
        rstart = region->GetRegionStart();
        offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
        if (region->GetRegionAllocPtr() > rstart) {
            allocOff = region->GetRegionAllocPtr() - rstart;
        }
        rs = static_cast<unsigned>(region->GetRouteState());
        young = static_cast<unsigned>(region->IsYoungRegion());
        survived = static_cast<unsigned>(region->IsSurvivedObject(offset));
        live = region->GetLiveByteCount();
    }
    if (obj != nullptr) {
        objFwd = static_cast<unsigned>(obj->IsForwarded());
    }
    const char* kind = "?";
    size_t hostStart = 0;
    size_t hostSz = 0;
    size_t walkStarts = 0;
    size_t walkBreakOff = 0;
    ClassifyOffset(region, offset, kind, hostStart, hostSz, walkStarts, walkBreakOff);
    LOG(RTLOG_ERROR,
        "[GCV2][tipwho] soft n=%zu branch=%s kind=%s obj=%p to=%p region=%p rstart=%#zx off=%zu "
        "allocOff=%zu rs=%u young=%u live=%zu surv=%u objFwd=%u hostStart=%zu hostSz=%zu "
        "walkStarts=%zu walkBreak=%zu",
        g_softN.load(std::memory_order_relaxed), branch != nullptr ? branch : "?", kind, obj, toObj, region,
        rstart, offset, allocOff, rs, young, live, survived, objFwd, hostStart, hostSz, walkStarts, walkBreakOff);
}

void NoteVisitGate(BaseObject* obj, RegionInfo* region, size_t offset, size_t position, const char* reason,
                   uintptr_t tipAddr, uint64_t tipWord0, uint64_t tipWord1, size_t nextSurvOff, size_t survAfter,
                   unsigned survAtBreak, size_t walkSteps, size_t prevOff, size_t prevSz, uintptr_t prevTip,
                   unsigned prevSurv)
{
    if (!Enabled()) {
        return;
    }
    g_visitGateN.fetch_add(1, std::memory_order_relaxed);
    // Always log when there are survived bits past the break (the permanent-hole shape);
    // otherwise apply sample cap.
    bool interesting = (survAfter > 0) || (offset == 6424) || (survAtBreak != 0);
    if (!interesting && !SampleOk()) {
        return;
    }
    size_t rstart = region != nullptr ? region->GetRegionStart() : 0;
    size_t allocOff = 0;
    unsigned rs = 0;
    unsigned young = 0;
    size_t live = 0;
    unsigned objState = 0;
    if (region != nullptr) {
        if (region->GetRegionAllocPtr() > rstart) {
            allocOff = region->GetRegionAllocPtr() - rstart;
        }
        rs = static_cast<unsigned>(region->GetRouteState());
        young = static_cast<unsigned>(region->IsYoungRegion());
        live = region->GetLiveByteCount();
    }
    if (obj != nullptr && Heap::IsHeapAddress(obj)) {
        objState = static_cast<unsigned>(obj->GetObjectState().GetStateCode());
    }
    // By construction of VisitLive size-walk, offset == sum of prior GetAllocSize steps.
    // isStart_mark = surv bit at break; isStart_walk = always 1 if we arrived via size sum.
    // 乙2 if survAtBreak=0 (walk landed where mark never claimed a start).
    // 乙1 if survAtBreak=1 (mark agrees this is a start; tip itself is bad).
    unsigned offAlign8 = (offset % 8 == 0) ? 1U : 0U;
    unsigned prevSzAlign8 = (prevSz % 8 == 0) ? 1U : 0U;
    LOG(RTLOG_ERROR,
        "[GCV2][tipwho] walk_break site=VisitLiveObjectsUntilFalse:PlausibleGate "
        "line=RegionManager.cpp:510 reason=%s obj=%p region=%p rstart=%#zx off=%zu pos=%#zx "
        "allocOff=%zu rs=%u young=%u live=%zu tip=%#zx tipW0=%#llx tipW1=%#llx objState=%u "
        "nextSurvOff=%zu survAfter=%zu size_read=NOT_CALLED "
        "survAtBreak=%u offAlign8=%u walkSteps=%zu prevOff=%zu prevSz=%zu prevSzAlign8=%u "
        "prevTip=%#zx prevSurv=%u isStart_walk=1 isStart_mark=%u",
        reason != nullptr ? reason : "?", obj, region, rstart, offset, position, allocOff, rs, young, live,
        static_cast<size_t>(tipAddr), static_cast<unsigned long long>(tipWord0),
        static_cast<unsigned long long>(tipWord1), objState, nextSurvOff, survAfter, survAtBreak, offAlign8,
        walkSteps, prevOff, prevSz, prevSzAlign8, prevTip, prevSurv, survAtBreak);
    // Attribute the first survived bit past the break (classic 6424→19400).
    if (nextSurvOff > 0) {
        NotePaintLookup(region, nextSurvOff, "walk_break.nextSurv");
    }
    // Attribute the break offset itself (乙1: paint start bit; 乙2: miss or multi-bit cover).
    NotePaintLookup(region, offset, "walk_break.atBreak");
}

void NotePaint(RegionInfo* region, BaseObject* obj, size_t offset, size_t objSize, const char* site, void* ra0,
               void* ra1)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    size_t seq = g_paintSeq.fetch_add(1, std::memory_order_relaxed);
    PaintRec& rec = g_paintRing[seq % kPaintRing];
    rec.region = reinterpret_cast<uintptr_t>(region);
    rec.rstart = region->GetRegionStart();
    rec.obj = reinterpret_cast<uintptr_t>(obj);
    rec.offset = offset;
    rec.objSize = objSize;
    rec.gcCount = g_gcCount;
    rec.phase = static_cast<unsigned>(Heap::GetHeap().GetGCPhase());
    rec.ra0 = ra0;
    rec.ra1 = ra1;
    if (site != nullptr) {
        size_t i = 0;
        for (; i + 1 < sizeof(rec.site) && site[i] != '\0'; ++i) {
            rec.site[i] = site[i];
        }
        rec.site[i] = '\0';
    } else {
        rec.site[0] = '\0';
    }
    g_paintN.fetch_add(1, std::memory_order_relaxed);
}

void NotePaintLookup(RegionInfo* region, size_t queryOff, const char* context)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    uintptr_t regKey = reinterpret_cast<uintptr_t>(region);
    uintptr_t rstart = region->GetRegionStart();
    size_t seq = g_paintSeq.load(std::memory_order_relaxed);
    size_t n = seq < kPaintRing ? seq : kPaintRing;
    // Newest first.
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (seq - 1 - i) % kPaintRing;
        const PaintRec& rec = g_paintRing[idx];
        if (rec.region != regKey && rec.rstart != rstart) {
            continue;
        }
        // Exact start bit, or multi-bit range covering queryOff.
        bool hit = (rec.offset == queryOff) ||
            (rec.offset < queryOff && queryOff < rec.offset + rec.objSize);
        if (!hit) {
            continue;
        }
        unsigned multi = (rec.offset != queryOff) ? 1U : 0U;
        LOG(RTLOG_ERROR,
            "[GCV2][tipwho] paint_hit ctx=%s queryOff=%zu paintOff=%zu paintSz=%zu multiBit=%u "
            "obj=%#zx region=%#zx rstart=%#zx site=%s phase=%u(%s) gc=%zu ra0=%p ra1=%p paintN=%zu",
            context != nullptr ? context : "?", queryOff, rec.offset, rec.objSize, multi, rec.obj, rec.region,
            rec.rstart, rec.site, rec.phase, Collector::GetGCPhaseName(static_cast<GCPhase>(rec.phase)),
            rec.gcCount, rec.ra0, rec.ra1, g_paintN.load(std::memory_order_relaxed));
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][tipwho] paint_miss ctx=%s queryOff=%zu region=%p rstart=%#zx paintN=%zu ring=%zu",
        context != nullptr ? context : "?", queryOff, region, rstart, g_paintN.load(std::memory_order_relaxed),
        n);
}

void NotePrePublish(RegionInfo* region)
{
    if (!Enabled() || region == nullptr || region->IsLargeRegion()) {
        return;
    }
    g_prePubN.fetch_add(1, std::memory_order_relaxed);
    uintptr_t rstart = region->GetRegionStart();
    uintptr_t alloc = region->GetRegionAllocPtr();
    size_t starts = 0;
    size_t copied = 0;
    size_t uncopied = 0;
    size_t gateBreak = 0;
    size_t firstUncOff = SIZE_MAX;
    BaseObject* firstUnc = nullptr;
    uintptr_t position = rstart;
    size_t offset = 0;
    while (position < alloc) {
        BaseObject* obj = from_region_addr(position);
        TypeInfo* tip = obj->GetTypeInfo();
        uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
        if (tip == nullptr || tipAddr < 0x1000U || (tipAddr & 0x7U) != 0 || Heap::IsHeapAddress(tipAddr)) {
            ++gateBreak;
            break;
        }
        size_t sz = RegionSpace::GetAllocSize(*obj);
        if (sz == 0) {
            ++gateBreak;
            break;
        }
        if (region->IsSurvivedObject(offset)) {
            ++starts;
            if (obj->IsForwarded()) {
                ++copied;
            } else {
                ++uncopied;
                if (firstUnc == nullptr) {
                    firstUnc = obj;
                    firstUncOff = offset;
                }
            }
        }
        position += sz;
        offset += sz;
    }
    // Multi-bit interior / orphan marks: sample every 8B bit; count marked bits that are not size-walk starts.
    size_t interior = 0;
    size_t orphan = 0;
    size_t regionSz = region->GetRegionSize();
    LiveInfo* li = region->GetLiveInfo0ForProbe();
    if (li == nullptr) {
        li = region->GetLiveInfo();
    }
    RegionBitmap* mb = li != nullptr ? li->markBitmap : nullptr;
    if (mb != nullptr && regionSz >= kMarkedBytesPerBit) {
        size_t bitCnt = regionSz / kMarkedBytesPerBit;
        // Bound scan cost: small regions only, or first 8KiB of bits.
        size_t maxBits = bitCnt;
        if (maxBits > 1024) {
            maxBits = 1024;
        }
        for (size_t bi = 0; bi < maxBits; ++bi) {
            size_t off = bi * kMarkedBytesPerBit;
            if (!mb->IsMarked(off)) {
                continue;
            }
            // Re-walk classification is expensive; approximate: if not survived as start in size-walk
            // we already counted starts via IsSurvivedObject at starts only. A marked bit that is not
            // a start offset of any size-walk object is interior or orphan.
            const char* kind = "?";
            size_t hostStart = 0;
            size_t hostSz = 0;
            size_t walkStarts = 0;
            size_t walkBreakOff = 0;
            ClassifyOffset(region, off, kind, hostStart, hostSz, walkStarts, walkBreakOff);
            if (std::strcmp(kind, "interior") == 0) {
                ++interior;
            } else if (std::strcmp(kind, "orphan") == 0 || std::strcmp(kind, "walk_break") == 0 ||
                       std::strcmp(kind, "past_alloc") == 0) {
                ++orphan;
            }
        }
    }
    if (uncopied > 0) {
        g_uncopiedStarts.fetch_add(uncopied, std::memory_order_relaxed);
    }
    if (interior > 0) {
        g_interiorMarks.fetch_add(interior, std::memory_order_relaxed);
    }
    if (orphan > 0) {
        g_orphanMarks.fetch_add(orphan, std::memory_order_relaxed);
    }
    // Always log when uncopied or interior/orphan present; otherwise sample lightly.
    bool interesting = (uncopied > 0) || (interior > 0) || (orphan > 0) || (gateBreak > 0);
    if (!interesting && !SampleOk()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][tipwho] pre_pub region=%p rstart=%#zx allocOff=%zu live=%zu rs=%u young=%u "
        "starts=%zu copied=%zu uncopied=%zu gateBreak=%zu interiorBits=%zu orphanBits=%zu "
        "firstUnc=%p firstUncOff=%zu",
        region, rstart, static_cast<size_t>(alloc - rstart), region->GetLiveByteCount(),
        static_cast<unsigned>(region->GetRouteState()), static_cast<unsigned>(region->IsYoungRegion()), starts,
        copied, uncopied, gateBreak, interior, orphan, firstUnc,
        firstUncOff == SIZE_MAX ? 0 : firstUncOff);
}

void NotePermHole(BaseObject* from, RegionInfo* region, BaseObject* geometricTo, const char* reason)
{
    if (!Enabled()) {
        // Still emit one line under permanent hole even if gate off — hole is rare and fatal.
        // Keep it cheap: only classify when region+from present.
    }
    g_permN.fetch_add(1, std::memory_order_relaxed);
    size_t offset = 0;
    size_t rstart = 0;
    size_t allocOff = 0;
    unsigned rs = 0;
    unsigned young = 0;
    unsigned survived = 0;
    unsigned objFwd = 0;
    unsigned toValid = 0;
    size_t live = 0;
    if (region != nullptr) {
        rstart = region->GetRegionStart();
        if (from != nullptr) {
            offset = region->GetAddressOffset(reinterpret_cast<MAddress>(from));
            survived = static_cast<unsigned>(region->IsSurvivedObject(offset));
        }
        if (region->GetRegionAllocPtr() > rstart) {
            allocOff = region->GetRegionAllocPtr() - rstart;
        }
        rs = static_cast<unsigned>(region->GetRouteState());
        young = static_cast<unsigned>(region->IsYoungRegion());
        live = region->GetLiveByteCount();
    }
    if (from != nullptr) {
        objFwd = static_cast<unsigned>(from->IsForwarded());
    }
    if (geometricTo != nullptr && Heap::IsHeapAddress(geometricTo)) {
        toValid = static_cast<unsigned>(geometricTo->IsValidObject());
    }
    const char* kind = "?";
    size_t hostStart = 0;
    size_t hostSz = 0;
    size_t walkStarts = 0;
    size_t walkBreakOff = 0;
    ClassifyOffset(region, offset, kind, hostStart, hostSz, walkStarts, walkBreakOff);
    // Multi-bit: host marked range contains offset but offset is not hostStart.
    unsigned multiBit = (std::strcmp(kind, "interior") == 0) ? 1U : 0U;
    unsigned orphan = (std::strcmp(kind, "orphan") == 0 || std::strcmp(kind, "walk_break") == 0) ? 1U : 0U;
    unsigned startKind = (std::strcmp(kind, "start") == 0) ? 1U : 0U;
    LOG(RTLOG_ERROR,
        "[GCV2][tipwho] permhole reason=%s kind=%s multiBit=%u orphan=%u start=%u from=%p to=%p "
        "toValid=%u region=%p rstart=%#zx off=%zu allocOff=%zu rs=%u young=%u live=%zu surv=%u "
        "objFwd=%u hostStart=%zu hostSz=%zu walkStarts=%zu walkBreak=%zu softN=%zu visitGateN=%zu "
        "prePubN=%zu uncopiedStarts=%zu interiorBits=%zu orphanBits=%zu",
        reason != nullptr ? reason : "?", kind, multiBit, orphan, startKind, from, geometricTo, toValid, region,
        rstart, offset, allocOff, rs, young, live, survived, objFwd, hostStart, hostSz, walkStarts, walkBreakOff,
        g_softN.load(std::memory_order_relaxed), g_visitGateN.load(std::memory_order_relaxed),
        g_prePubN.load(std::memory_order_relaxed), g_uncopiedStarts.load(std::memory_order_relaxed),
        g_interiorMarks.load(std::memory_order_relaxed), g_orphanMarks.load(std::memory_order_relaxed));
}

} // namespace TipWhoDiag
} // namespace MapleRuntime
