// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/ZgcInvariants.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Common/ColourMask.h"
#include "Heap/Verify/TraceClear.h"

namespace MapleRuntime {
namespace ZgcInvariants {

// Compile-time gate, not an environment variable: the campaign cut MRT_GCV2_* from 190 to 3, and a
// new one would be a fourth.  I1 costs one relaxed load on the barrier's return path.
constexpr bool kInvariantsOn = true;

static std::atomic<uint64_t> g_i1Checked{ 0 };
static std::atomic<uint64_t> g_i1Violations{ 0 };
static std::atomic<uint64_t> g_i1Forwarded{ 0 };
static std::atomic<uint64_t> g_i1ZeroHeader{ 0 };

bool Enabled() { return kInvariantsOn; }

void CheckLoadGoodTarget(BaseObject* target, const Collector& collector, uint8_t phase)
{
    if (!kInvariantsOn) {
        return;
    }
    (void)collector;
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    const uint64_t seen = g_i1Checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen == 1) {
        // The zero case has to be emitted too, or a clean run and a dead probe look identical --
        // that mistake cost a whole turn earlier when an install-time census of all-zero counters
        // was read as "the read barrier never self-heals".
        LOG(RTLOG_ERROR, "[ZGCINV][I1] armed phase=%u", phase);
    }

    // StateWord.h:174-178 lays the header out as typeInfoLow32(0-31), typeInfoHigh16(32-47),
    // objectState(48-63) with stateCode:2 at bits 48-49; StateWord.h:22-30 gives FORWARDED = 3.
    // The runtime reads the two address bitfields and is immune, but the compiler loads the whole
    // word, so any non-zero state code lands inside the address it then dereferences.
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    const unsigned stateCode = static_cast<unsigned>((hdr >> 48) & 0x3u);
    const uint64_t typeInfo = hdr & 0xffffffffffffull;
    const bool forwarded = stateCode == 3u; // ObjectState::FORWARDED
    const bool zeroHeader = typeInfo == 0;
    if (!forwarded && !zeroHeader) {
        return;
    }

    if (forwarded) {
        g_i1Forwarded.fetch_add(1, std::memory_order_relaxed);
    }
    if (zeroHeader) {
        g_i1ZeroHeader.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t v = g_i1Violations.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((v & (v - 1)) != 0) { // powers of two: the subject dies by SIGSEGV, so there is no atexit
        return;
    }
    LOG(RTLOG_ERROR,
        "[ZGCINV][I1] violation=%lu of checked=%lu target=%p stateCode=%u typeInfo=%#lx phase=%u kind=%s", v, seen,
        static_cast<void*>(target), stateCode, typeInfo, phase, zeroHeader ? "zero-header" : "forwarded");
}

// State census: instead of testing one hypothesis at a time, enumerate what states actually occur.
//
// ZGC's legal state space is written down (zAddress.hpp:59-130): the remap bit is a bijection with
// the (RemappedYoung[0,1], RemappedOld[0,1]) epoch pair --
//   Old0&Young0 = Remapped00   Old0&Young1 = Remapped01
//   Old1&Young0 = Remapped10   Old1&Young1 = Remapped11
// and our masks start from the identical values (WCollector.h:139-140 = ZGC's Young0/Old0), so a
// slot's colour records the epoch pair it was painted at.  What is *not* written down anywhere is
// which (slot colour x target state) combinations the design excludes -- that is exactly the part
// twelve hypotheses failed to guess.  So record the cross product and let the table say it.
//
// Key packs: slotColour(3) | goodColour(3) | stateCode(2) | routeState(3) | ghost(1) | young(1) = 13 bits.
// ColourIndex returns 0..4 (0 = no remap bit at all), so three bits each -- masking to two would fold
// Remapped11 back onto "plain", which is precisely the distinction this table exists to make.
static std::atomic<uint64_t> g_census[1u << 13] = {};
static std::atomic<uint64_t> g_censusTotal{ 0 };
static std::atomic<uint64_t> g_illegalHits{ 0 };
static std::atomic<uint64_t> g_illegalByPhase[7] = {};
static std::atomic<uint64_t> g_healReturnChecked{ 0 };
static std::atomic<uint64_t> g_healReturnFailures{ 0 };
static std::atomic<bool> g_summaryRegistered{ false };
uint64_t WCollectorFlipSeqForProbe();
BaseObject* ProbeFindToVersion(BaseObject* obj);

static const char* PhaseName(uint8_t phase)
{
    static constexpr const char* kNames[] = { "stw", "idle", "enum", "trace", "post-trace", "preforward", "forward" };
    return phase < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[phase] : "unknown";
}

static void EnsureSummaryAtExit()
{
    bool expected = false;
    if (g_summaryRegistered.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit([]() { DumpSummary("atexit"); });
    }
}

static unsigned ColourIndex(uint64_t remapBits)
{
    // Remapped00/01/10/11 are one-hot at REMAP_COLOUR_SHIFT+0..3; 0 means "no remap bit at all".
    if (remapBits == 0) {
        return 0;
    }
    unsigned idx = 0;
    while ((remapBits & 1) == 0 && idx < 63) {
        remapBits >>= 1;
        ++idx;
    }
    return (idx & 0x3u) + 1u; // 1..4, leaving 0 for "plain / no colour"
}

static void NoteStateImpl(uintptr_t slotRaw, uintptr_t slotRawSecondRead, uintptr_t goodMask, BaseObject* target,
                          uint8_t phase, bool probeToVersion)
{
    if (!kInvariantsOn || target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    EnsureSummaryAtExit();
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    const unsigned stateCode = static_cast<unsigned>((hdr >> 48) & 0x3u);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    const unsigned routeState = region == nullptr ? 7u : static_cast<unsigned>(region->GetRouteState()) & 0x7u;
    const unsigned ghost = (region != nullptr && region->IsGhostFromRegion()) ? 1u : 0u;
    const unsigned young = (region != nullptr && region->IsYoungRegion()) ? 1u : 0u;
    const unsigned slotC = ColourIndex((slotRaw >> REMAP_COLOUR_SHIFT) & 0xfu) & 0x7u;
    const unsigned goodC = ColourIndex((goodMask >> REMAP_COLOUR_SHIFT) & 0xfu) & 0x7u;
    const unsigned key = slotC | (goodC << 3) | (stateCode << 6) | (routeState << 8) | (ghost << 11) | (young << 12);
    g_census[key].fetch_add(1, std::memory_order_relaxed);

    // The one tuple the state table says ZGC excludes: colour good, target forwarded, region ghost.
    // 53 occurrences in 24M means it can afford expensive logging.  flipSeq separates the two
    // remaining stories: a colour cannot have recycled into good before two publications, so a hit
    // at flipSeq < 2 proves the slot was painted *after* the target had already moved.
    if (slotC == goodC && stateCode == 3u && ghost == 1u) {
        const uint64_t n = g_illegalHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (phase < (sizeof(g_illegalByPhase) / sizeof(g_illegalByPhase[0]))) {
            g_illegalByPhase[phase].fetch_add(1, std::memory_order_relaxed);
        }
        // A pre-kStaleGuard measurement recorded slot/return disagreement in 54 of 68 hits.  This
        // post-return observation cannot prove the barrier wrote that slot value: another writer
        // may finish before both reads.  ZBarrier avoids the ambiguity structurally by deriving the
        // healing pointer and return value from one good address; AssertHealMatchesReturn checks
        // that direct data-flow contract at this runtime's write-back boundary.
        const uintptr_t slotRaw2 = slotRawSecondRead;
        // Two stable reads exclude only a write between the two probe loads.  Keep to-version
        // classification for historical compatibility, but do not infer who wrote the slot.
        BaseObject* toVer = probeToVersion ? ProbeFindToVersion(target) : nullptr;
        const unsigned slotIsToVersion =
            (toVer != nullptr &&
             (reinterpret_cast<uintptr_t>(toVer) & 0xffffffffffffull) == (slotRaw & 0xffffffffffffull))
            ? 1u : 0u;
        if (n <= 24) {
            LOG(RTLOG_ERROR,
                "[ZGCINV][illegal] n=%lu phase=%u barrier=%s flipSeq=%lu target=%p slotRaw=%#lx slotRaw2=%#lx "
                "addrMatch=%u toVer=%p slotIsToVersion=%u route=%u young=%u",
                n, phase, PhaseName(phase), WCollectorFlipSeqForProbe(), static_cast<void*>(target),
                static_cast<unsigned long>(slotRaw),
                static_cast<unsigned long>(slotRaw2),
                ((slotRaw & 0xffffffffffffull) == (reinterpret_cast<uintptr_t>(target) & 0xffffffffffffull)) ? 1u : 0u,
                static_cast<void*>(toVer), slotIsToVersion, routeState, young);
        }
    }
    const uint64_t n = g_censusTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0 && n >= (1u << 22)) {
        DumpCensus("periodic");
    }
}

void NoteState(uintptr_t slotRaw, uintptr_t slotRawSecondRead, uintptr_t goodMask, BaseObject* target, uint8_t phase)
{
    NoteStateImpl(slotRaw, slotRawSecondRead, goodMask, target, phase, true);
}

uint64_t IllegalHitCount() { return g_illegalHits.load(std::memory_order_relaxed); }

uint64_t InjectIllegalTupleForTest(uintptr_t slotRaw, uintptr_t goodMask, BaseObject* target, uint8_t phase)
{
    const uint64_t before = IllegalHitCount();
    NoteStateImpl(slotRaw, slotRaw, goodMask, target, phase, false);
    return IllegalHitCount() - before;
}

void AssertHealMatchesReturn(uintptr_t healRaw, BaseObject* returned, uint16_t site)
{
    EnsureSummaryAtExit();
    g_healReturnChecked.fetch_add(1, std::memory_order_relaxed);
    constexpr uintptr_t kAddressMask = (uintptr_t(1) << 48) - 1u;
    const uintptr_t healAddress = healRaw & kAddressMask;
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(returned);
    if (UNLIKELY(healAddress != returnAddress)) {
        g_healReturnFailures.fetch_add(1, std::memory_order_relaxed);
    }
    CHECK_DETAIL(healAddress == returnAddress,
                 "[ZGCINV][heal-return] site=%u healRaw=%#lx healAddress=%#lx returned=%p",
                 static_cast<unsigned>(site), static_cast<unsigned long>(healRaw),
                 static_cast<unsigned long>(healAddress), static_cast<void*>(returned));
}

static std::atomic<uint64_t> g_fastAcceptBad{ 0 };

void NoteFastPathAccept(uintptr_t slotRaw, BaseObject* target)
{
    if (!kInvariantsOn || target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    const uint64_t hdr = __atomic_load_n(reinterpret_cast<const uint64_t*>(target), __ATOMIC_RELAXED);
    if (((hdr >> 48) & 0x3u) != 3u) { // not FORWARDED
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    const unsigned ghost = (region != nullptr && region->IsGhostFromRegion()) ? 1u : 0u;
    const unsigned routeState = region == nullptr ? 7u : static_cast<unsigned>(region->GetRouteState()) & 0x7u;
    const uint64_t n = g_fastAcceptBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 24 && (n & (n - 1)) != 0) {
        return;
    }
    LOG(RTLOG_ERROR, "[ZGCINV][fastfrom] n=%lu flipSeq=%lu slotRaw=%#lx colourBits=%#lx loadBad=%#lx ghost=%u route=%u",
        n, WCollectorFlipSeqForProbe(), static_cast<unsigned long>(slotRaw),
        static_cast<unsigned long>(slotRaw & REMAP_COLOUR_MASK), static_cast<unsigned long>(::g_cjLoadBadMask), ghost,
        routeState);
}

static std::atomic<uint64_t> g_staleGuardFired{ 0 };

static std::atomic<uint64_t> g_staleGuardZero{ 0 };
static std::atomic<uint64_t> g_staleGuardUnresolved{ 0 };

void NoteStaleGuardFired(bool zeroHeader, bool resolved, BaseObject* target, BaseObject* holder, const void* slot)
{
    if (!kInvariantsOn) {
        return;
    }
    const uint64_t n = g_staleGuardFired.fetch_add(1, std::memory_order_relaxed) + 1;
    if (zeroHeader) {
        g_staleGuardZero.fetch_add(1, std::memory_order_relaxed);
    }
    if (!resolved) {
        g_staleGuardUnresolved.fetch_add(1, std::memory_order_relaxed);
    }
    // An unresolved zero-header target is the one the barrier cannot repair: nothing to forward to,
    // so the mutator gets an object full of zeroes and faults on the first field it dereferences.
    // Count it separately -- it is a reclaim/mark defect surfacing here, not a barrier defect.
    // oracleblack round 10 (flood guard): the zeroHeader&&!resolved arm used to bypass the
    // power-of-two throttle entirely; a consumer retry loop on one unresolvable address then
    // prints at ~90MB/s and fills the disk before any timeout fires. Keep the first 64 such
    // events verbatim (forensics) and fall back to the same 2^n cadence afterwards.
    static std::atomic<uint64_t> g_staleGuardUnusableLogged{ 0 };
    bool logUnusable = false;
    if (zeroHeader && !resolved) {
        const uint64_t u = g_staleGuardUnusableLogged.fetch_add(1, std::memory_order_relaxed) + 1;
        logUnusable = (u <= 64) || ((u & (u - 1)) == 0);
    }
    if ((n & (n - 1)) == 0 || logUnusable) {
        // Which region the unusable target sits in is the whole question for the zero-header arm:
        // nothing to forward to means the object was reclaimed, and a region's own type says
        // whether that reclamation already happened (GARBAGE/FREE) or whether the payload was
        // cleared while the region is still nominally in use.  Those need different fixes, so
        // record the region rather than assume either.
        RegionInfo* region = (target != nullptr && Heap::IsHeapAddress(target))
            ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target))
            : nullptr;
        const unsigned rtype = region == nullptr ? 99u : static_cast<unsigned>(region->GetRegionType());
        const unsigned route = region == nullptr ? 9u : static_cast<unsigned>(region->GetRouteState());
        const unsigned garbage = (region != nullptr && region->IsGarbageRegion()) ? 1u : 0u;
        const unsigned freeR = (region != nullptr && region->IsFreeRegion()) ? 1u : 0u;
        const unsigned ghostR = (region != nullptr && region->IsGhostFromRegion()) ? 1u : 0u;
        // pastalloc: a zero header inside a *live* allocation region (THREAD_LOCAL / RECENT_FULL,
        // garbage=0 free=0 ghost=0) is not a collected object.  Publication ordering was the
        // obvious next guess and it is wrong on this target: x86-64 is TSO, so store-store and
        // load-load are already ordered and release adds nothing -- switching StoreColoured's
        // default from relaxed to release moved nothing (15 -> 18 unusable, crashes 8/10 -> 10/10).
        //
        // What is left is that the address is not an object start at all: TLAB space past the
        // region's allocation pointer is simply blank, and a pointer into it reads as a zero
        // header.  allocPtr says which side of that line the target is on.
        const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
        const uintptr_t allocPtr = region == nullptr ? 0 : region->GetRegionAllocPtr();
        const uintptr_t startPtr = region == nullptr ? 0 : region->GetRegionStart();
        const unsigned pastAlloc = (region != nullptr && addr >= allocPtr) ? 1u : 0u;
        const unsigned inRange = (region != nullptr && addr >= startPtr && addr < allocPtr) ? 1u : 0u;
        // Direct evidence of who zeroed this address, from the ring recorded at clear time.
        char clearInfo[384] = {};
        if (zeroHeader && !resolved) {
            (void)TraceClear::Lookup(static_cast<MAddress>(addr), clearInfo, sizeof(clearInfo));
        }
        unsigned hgen = 0;
        unsigned htype = 99;
        unsigned halloc = 0;
        unsigned hlive = 0;
        unsigned hyoungMark = 0;
        unsigned holdMark = 0;
        unsigned inRemset = 0;
        unsigned edge = 0;
        if (holder != nullptr && Heap::IsHeapAddress(holder)) {
            RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (hr != nullptr && !hr->IsFreeRegion() && !hr->IsGarbageRegion()) {
                htype = static_cast<unsigned>(hr->GetRegionType());
                const bool young = hr->IsYoungRegion();
                hgen = young ? 1u : 2u;
                const RegionInfo::RegionType ht = hr->GetRegionType();
                halloc = (hr->HasMarkStartAllocGap() || hr->IsToRegion() || hr->IsThreadLocalRegion() ||
                          ht == RegionInfo::RegionType::RECENT_FULL_REGION ||
                          ht == RegionInfo::RegionType::RECENT_LARGE_REGION || hr->IsPinnedRegion())
                    ? 1u
                    : 0u;
                if (young) {
                    hyoungMark = hr->IsMarkedObject(hr->GetMarkView<Generation::Young>(), holder) ? 1u : 0u;
                } else {
                    holdMark = hr->IsMarkedObject(hr->GetMarkView<Generation::Old>(), holder) ? 1u : 0u;
                }
                hlive = (halloc || hyoungMark || holdMark) ? 1u : 0u;
                const bool tgtYoung = std::strstr(clearInfo, " y=1 ") != nullptr ||
                    std::strstr(clearInfo, "y=1 rtype") != nullptr;
                if (young && tgtYoung) {
                    edge = 3;
                } else if (young) {
                    edge = 4;
                } else if (tgtYoung) {
                    edge = 1;
                } else {
                    edge = 2;
                }
            }
        }
        if (slot != nullptr && Heap::IsHeapAddress(slot)) {
            inRemset = Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(slot)) ? 1u : 0u;
        }
        LOG(RTLOG_ERROR,
            "[ZGCINV][staleguard] fired=%lu zeroHeader=%lu unresolved=%lu thisZero=%u thisResolved=%u "
            "rtype=%u route=%u garbage=%u free=%u ghost=%u pastAlloc=%u inRange=%u off=%#lx "
            "hgen=%u htype=%u halloc=%u hlive=%u hyMark=%u hoMark=%u edge=%u remset=%u clear=%s",
            n, g_staleGuardZero.load(std::memory_order_relaxed),
            g_staleGuardUnresolved.load(std::memory_order_relaxed), zeroHeader ? 1u : 0u, resolved ? 1u : 0u, rtype,
            route, garbage, freeR, ghostR, pastAlloc, inRange,
            static_cast<unsigned long>(region == nullptr ? 0 : addr - startPtr), hgen, htype, halloc, hlive,
            hyoungMark, holdMark, edge, inRemset, clearInfo[0] == '\0' ? "-" : clearInfo);
    }
}

void DumpCensus(const char* why)
{
    if (!kInvariantsOn) {
        return;
    }
    LOG(RTLOG_ERROR, "[ZGCINV][census] why=%s total=%lu", why == nullptr ? "?" : why,
        g_censusTotal.load(std::memory_order_relaxed));
    for (unsigned k = 0; k < (1u << 13); ++k) {
        const uint64_t c = g_census[k].load(std::memory_order_relaxed);
        if (c == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[ZGCINV][state] slotC=%u goodC=%u sc=%u route=%u ghost=%u young=%u n=%lu", k & 0x7u,
            (k >> 3) & 0x7u, (k >> 6) & 0x3u, (k >> 8) & 0x7u, (k >> 11) & 0x1u, (k >> 12) & 0x1u, c);
    }
}

void DumpSummary(const char* why)
{
    if (!kInvariantsOn) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[ZGCINV][summary] why=%s i1_checked=%lu i1_violations=%lu i1_forwarded=%lu i1_zero_header=%lu "
        "illegal=%lu stw=%lu idle=%lu enum=%lu trace=%lu post_trace=%lu preforward=%lu forward=%lu "
        "heal_return_checked=%lu heal_return_failures=%lu",
        why == nullptr ? "?" : why, g_i1Checked.load(std::memory_order_relaxed),
        g_i1Violations.load(std::memory_order_relaxed), g_i1Forwarded.load(std::memory_order_relaxed),
        g_i1ZeroHeader.load(std::memory_order_relaxed), g_illegalHits.load(std::memory_order_relaxed),
        g_illegalByPhase[0].load(std::memory_order_relaxed), g_illegalByPhase[1].load(std::memory_order_relaxed),
        g_illegalByPhase[2].load(std::memory_order_relaxed), g_illegalByPhase[3].load(std::memory_order_relaxed),
        g_illegalByPhase[4].load(std::memory_order_relaxed), g_illegalByPhase[5].load(std::memory_order_relaxed),
        g_illegalByPhase[6].load(std::memory_order_relaxed), g_healReturnChecked.load(std::memory_order_relaxed),
        g_healReturnFailures.load(std::memory_order_relaxed));
}

} // namespace ZgcInvariants
} // namespace MapleRuntime
