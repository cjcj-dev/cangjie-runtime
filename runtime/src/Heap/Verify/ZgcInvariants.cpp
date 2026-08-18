// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/ZgcInvariants.h"

#include <atomic>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "Heap/Allocator/RegionInfo.h"
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
uint64_t WCollectorFlipSeqForProbe();
BaseObject* ProbeFindToVersion(BaseObject* obj);

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

void NoteState(uintptr_t slotRaw, uintptr_t slotRawSecondRead, uintptr_t goodMask, BaseObject* target)
{
    if (!kInvariantsOn || target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
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
        // The slot value and the handed-out pointer disagreed in 54 of 68 hits, which would mean the
        // barrier healed the slot to one object and returned another -- a contract ZGC does not
        // allow (ZBarrier::barrier computes one zaddress, colours it into the slot, and returns that
        // same zaddress).  Before believing that, rule out the mundane reading: the slot is shared,
        // and this probe reads it *after* the barrier returned, so another thread may simply have
        // written it in between.  slotRaw2 is a second read; if it differs from the first, the
        // disagreement is a race in the observation, not in the barrier.
        const uintptr_t slotRaw2 = slotRawSecondRead;
        // Two stable reads that disagree with the returned pointer rule out an observation race, so
        // the barrier really did hand back something other than what it wrote.  The shape suggests
        // which way round: the returned object carries FORWARDED and sits in a ghost-from region,
        // i.e. it is the *from* copy, while the slot holds a different address entirely.  If that
        // other address is this object's to-version, then the heal was correct and only the return
        // value is stale -- a local defect, not the cross-cycle staleness story.
        // ZGC cannot express this: ZBarrier::barrier computes one zaddress, colours it into the
        // slot, and returns that same zaddress.
        BaseObject* toVer = ProbeFindToVersion(target);
        const unsigned slotIsToVersion =
            (toVer != nullptr &&
             (reinterpret_cast<uintptr_t>(toVer) & 0xffffffffffffull) == (slotRaw & 0xffffffffffffull))
            ? 1u : 0u;
        if (n <= 24) {
            LOG(RTLOG_ERROR,
                "[ZGCINV][illegal] n=%lu flipSeq=%lu target=%p slotRaw=%#lx slotRaw2=%#lx addrMatch=%u "
                "toVer=%p slotIsToVersion=%u route=%u young=%u",
                n, WCollectorFlipSeqForProbe(), static_cast<void*>(target), static_cast<unsigned long>(slotRaw),
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

void NoteStaleGuardFired(bool zeroHeader, bool resolved, BaseObject* target)
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
    if ((n & (n - 1)) == 0 || (zeroHeader && !resolved)) {
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
        char clearInfo[256] = {};
        if (zeroHeader && !resolved) {
            (void)TraceClear::Lookup(static_cast<MAddress>(addr), clearInfo, sizeof(clearInfo));
        }
        LOG(RTLOG_ERROR,
            "[ZGCINV][staleguard] fired=%lu zeroHeader=%lu unresolved=%lu thisZero=%u thisResolved=%u "
            "rtype=%u route=%u garbage=%u free=%u ghost=%u pastAlloc=%u inRange=%u off=%#lx clear=%s",
            n, g_staleGuardZero.load(std::memory_order_relaxed),
            g_staleGuardUnresolved.load(std::memory_order_relaxed), zeroHeader ? 1u : 0u, resolved ? 1u : 0u, rtype,
            route, garbage, freeR, ghostR, pastAlloc, inRange,
            static_cast<unsigned long>(region == nullptr ? 0 : addr - startPtr),
            clearInfo[0] == '\0' ? "-" : clearInfo);
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
    LOG(RTLOG_ERROR, "[ZGCINV][summary] why=%s i1_checked=%lu i1_violations=%lu i1_forwarded=%lu i1_zero_header=%lu",
        why == nullptr ? "?" : why, g_i1Checked.load(std::memory_order_relaxed),
        g_i1Violations.load(std::memory_order_relaxed), g_i1Forwarded.load(std::memory_order_relaxed),
        g_i1ZeroHeader.load(std::memory_order_relaxed));
}

} // namespace ZgcInvariants
} // namespace MapleRuntime
