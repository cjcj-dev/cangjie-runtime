// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/PermWhoAdmit.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace PermWhoAdmit {
namespace {

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    return end == v ? def : static_cast<size_t>(n);
}

size_t MaxSamples()
{
    static const size_t n = EnvSizeT("MRT_GCV2_PERMWHO_ADMIT_MAX", 32);
    return n;
}

std::atomic<size_t> g_total{ 0 };
std::atomic<size_t> g_byState[8];
// FORWARDED (published) answers
std::atomic<size_t> g_fwdTotal{ 0 };
std::atomic<size_t> g_fwdGarbage{ 0 };
std::atomic<size_t> g_fwdFromNotFwd{ 0 };
std::atomic<size_t> g_fwdToNull{ 0 };
std::atomic<size_t> g_fwdToNotHeap{ 0 };
std::atomic<size_t> g_fwdToInvalid{ 0 };
std::atomic<size_t> g_fwdToValid{ 0 };
// Two ledgers at answer time. The permhole report prints live= from GetLiveByteCount()
// (the densify counter), while the reclaim decision reads the mark face via IsKnownEmpty().
// Counting both on the same answer says whether they agree.
std::atomic<size_t> g_fwdLiveZero{ 0 };
std::atomic<size_t> g_fwdKnownEmpty{ 0 };
std::atomic<size_t> g_fwdBooksSplit{ 0 };
// ROUTED (copy still in flight) answers, same three columns
std::atomic<size_t> g_rtdTotal{ 0 };
std::atomic<size_t> g_rtdToInvalid{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<bool> g_atexit{ false };
// Route planning: reservation (counter) vs placement (bitmap).
std::atomic<size_t> g_planTotal{ 0 };
std::atomic<size_t> g_planByOutcome[8];
std::atomic<size_t> g_planMismatch{ 0 };
std::atomic<size_t> g_planMismatchNotDensified{ 0 };
std::atomic<size_t> g_planCounterGreater{ 0 };
std::atomic<size_t> g_planBitmapGreater{ 0 };
std::atomic<size_t> g_planLogged{ 0 };
// Abandon arm: stale per-object receipts left in an exempted region.
std::atomic<size_t> g_abandonTotal{ 0 };
std::atomic<size_t> g_abandonWithStaleReceipt{ 0 };
std::atomic<size_t> g_abandonWalked{ 0 };
std::atomic<size_t> g_abandonForwarded{ 0 };
std::atomic<size_t> g_abandonLogged{ 0 };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { DumpSummary(); });
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_PERMWHO_ADMIT");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

void NoteRoute(RegionInfo* region, BaseObject* from, BaseObject* to)
{
    if (!Enabled() || region == nullptr || from == nullptr) {
        return;
    }
    EnsureAtexit();
    g_total.fetch_add(1, std::memory_order_relaxed);
    const RegionInfo::RouteState rs = region->GetRouteState();
    const unsigned rsIdx = static_cast<unsigned>(rs) < 8 ? static_cast<unsigned>(rs) : 7;
    g_byState[rsIdx].fetch_add(1, std::memory_order_relaxed);

    const bool published = (rs == RegionInfo::RouteState::FORWARDED);
    const bool routed = (rs == RegionInfo::RouteState::ROUTED);
    if (!published && !routed) {
        return;
    }
    const bool toNull = (to == nullptr);
    const bool toHeap = !toNull && Heap::IsHeapAddress(to);
    const bool toValid = toHeap && to->IsValidObject();
    if (routed) {
        g_rtdTotal.fetch_add(1, std::memory_order_relaxed);
        if (!toValid) {
            g_rtdToInvalid.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    g_fwdTotal.fetch_add(1, std::memory_order_relaxed);
    if (region->IsGarbageRegion()) {
        g_fwdGarbage.fetch_add(1, std::memory_order_relaxed);
    }
    const bool liveZero = (region->GetLiveByteCount() == 0);
    const bool knownEmpty = region->IsKnownEmpty();
    if (liveZero) {
        g_fwdLiveZero.fetch_add(1, std::memory_order_relaxed);
    }
    if (knownEmpty) {
        g_fwdKnownEmpty.fetch_add(1, std::memory_order_relaxed);
    }
    if (liveZero != knownEmpty) {
        g_fwdBooksSplit.fetch_add(1, std::memory_order_relaxed);
    }
    const bool fromFwd = from->IsForwarded();
    if (!fromFwd) {
        g_fwdFromNotFwd.fetch_add(1, std::memory_order_relaxed);
    }
    if (toNull) {
        g_fwdToNull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!toHeap) {
        g_fwdToNotHeap.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (toValid) {
        g_fwdToValid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The permhole precondition, observed without aborting.
    g_fwdToInvalid.fetch_add(1, std::memory_order_relaxed);
    size_t n = g_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > MaxSamples()) {
        return;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(to));
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    MAddress start = region->GetRegionStart();
    MAddress fromAddr = reinterpret_cast<MAddress>(from);
    size_t fromOff = fromAddr >= start ? static_cast<size_t>(fromAddr - start) : 0;
    LOG(RTLOG_ERROR,
        "[GCV2][permwho-admit] n=%zu phase=%u region=%p rtype=%u garbage=%u ghost=%u "
        "fromOff=%zu fromFwd=%u ghostSurv=%u knownEmpty=%u live=%zu "
        "to=%p toRtype=%u toGarbage=%u toFree=%u",
        n, static_cast<unsigned>(Heap::GetHeap().GetGCPhase()), region,
        static_cast<unsigned>(region->GetRegionType()), static_cast<unsigned>(region->IsGarbageRegion()),
        static_cast<unsigned>(region->IsGhostFromRegion()), fromOff, static_cast<unsigned>(fromFwd),
        static_cast<unsigned>(ghost != nullptr && ghost->IsSurvivedObject(fromOff)),
        static_cast<unsigned>(region->IsKnownEmpty()), region->GetLiveByteCount(), to,
        toRegion != nullptr ? static_cast<unsigned>(toRegion->GetRegionType()) : 0xffu,
        toRegion != nullptr ? static_cast<unsigned>(toRegion->IsGarbageRegion()) : 0xffu,
        toRegion != nullptr ? static_cast<unsigned>(toRegion->IsFreeRegion()) : 0xffu);
}

void NoteRoutePlan(RegionInfo* region, size_t fromBytes, unsigned densifyOutcome)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    g_planTotal.fetch_add(1, std::memory_order_relaxed);
    g_planByOutcome[densifyOutcome < 8 ? densifyOutcome : 7].fetch_add(1, std::memory_order_relaxed);
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    if (ghost == nullptr) {
        return;
    }
    const size_t bitmapBytes = ghost->RecomputeBitmapLiveBytes();
    if (bitmapBytes == fromBytes) {
        return;
    }
    g_planMismatch.fetch_add(1, std::memory_order_relaxed);
    if (densifyOutcome != 0) {
        g_planMismatchNotDensified.fetch_add(1, std::memory_order_relaxed);
    }
    if (fromBytes > bitmapBytes) {
        g_planCounterGreater.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_planBitmapGreater.fetch_add(1, std::memory_order_relaxed);
    }
    size_t n = g_planLogged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > MaxSamples()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][permwho-plan] n=%zu region=%p start=%#zx densify=%u reserveCounter=%zu "
        "placeBitmap=%zu delta=%zd young=%u small=%u knownEmpty=%u",
        n, region, region->GetRegionStart(), densifyOutcome, fromBytes, bitmapBytes,
        static_cast<ssize_t>(fromBytes) - static_cast<ssize_t>(bitmapBytes),
        static_cast<unsigned>(region->IsYoungRegion()), static_cast<unsigned>(region->IsSmallRegion()),
        static_cast<unsigned>(region->IsKnownEmpty()));
}

void NoteAbandon(RegionInfo* region, size_t walkedObjects, size_t forwardedObjects)
{
    if (!Enabled() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    g_abandonTotal.fetch_add(1, std::memory_order_relaxed);
    g_abandonWalked.fetch_add(walkedObjects, std::memory_order_relaxed);
    g_abandonForwarded.fetch_add(forwardedObjects, std::memory_order_relaxed);
    if (forwardedObjects > 0) {
        g_abandonWithStaleReceipt.fetch_add(1, std::memory_order_relaxed);
        size_t n = g_abandonLogged.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= MaxSamples()) {
            LOG(RTLOG_ERROR,
                "[GCV2][permwho-abandon] n=%zu region=%p start=%#zx walked=%zu forwarded=%zu "
                "live=%zu route=%u rtype=%u young=%u — Exempt keeps %zu stale receipts",
                n, region, region->GetRegionStart(), walkedObjects, forwardedObjects,
                region->GetLiveByteCount(), static_cast<unsigned>(region->GetRouteState()),
                static_cast<unsigned>(region->GetRegionType()),
                static_cast<unsigned>(region->IsYoungRegion()), forwardedObjects);
        }
    }
}

void DumpSummary()
{
    if (!Enabled()) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][permwho-abandon] atexit abandons=%zu withStaleReceipt=%zu walkedObjects=%zu "
                 "forwardedObjects=%zu\n",
                 g_abandonTotal.load(std::memory_order_relaxed),
                 g_abandonWithStaleReceipt.load(std::memory_order_relaxed),
                 g_abandonWalked.load(std::memory_order_relaxed),
                 g_abandonForwarded.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[GCV2][permwho-plan] atexit plans=%zu densified=%zu gate=%zu walk1=%zu nstarts0=%zu "
                 "malloc=%zu walk2broke=%zu walk2short=%zu mismatch=%zu mismatchNotDensified=%zu counterGT=%zu bitmapGT=%zu\n",
                 g_planTotal.load(std::memory_order_relaxed), g_planByOutcome[0].load(std::memory_order_relaxed),
                 g_planByOutcome[1].load(std::memory_order_relaxed), g_planByOutcome[2].load(std::memory_order_relaxed),
                 g_planByOutcome[3].load(std::memory_order_relaxed), g_planByOutcome[4].load(std::memory_order_relaxed),
                 g_planByOutcome[5].load(std::memory_order_relaxed), g_planByOutcome[6].load(std::memory_order_relaxed),
                 g_planMismatch.load(std::memory_order_relaxed),
                 g_planMismatchNotDensified.load(std::memory_order_relaxed),
                 g_planCounterGreater.load(std::memory_order_relaxed),
                 g_planBitmapGreater.load(std::memory_order_relaxed));
    std::fflush(stderr);
    std::fprintf(stderr,
                 "[GCV2][permwho-admit] atexit total=%zu byState[normal=%zu forwardable=%zu routing=%zu "
                 "routed=%zu compacted=%zu forwarded=%zu other=%zu] "
                 "ROUTED{total=%zu toInvalid=%zu} "
                 "FORWARDED{total=%zu garbageRegion=%zu fromNotFwd=%zu toNull=%zu toNotHeap=%zu "
                 "toInvalid=%zu toValid=%zu liveZero=%zu knownEmpty=%zu booksSplit=%zu}\n",
                 g_total.load(std::memory_order_relaxed), g_byState[0].load(std::memory_order_relaxed),
                 g_byState[1].load(std::memory_order_relaxed), g_byState[2].load(std::memory_order_relaxed),
                 g_byState[3].load(std::memory_order_relaxed), g_byState[4].load(std::memory_order_relaxed),
                 g_byState[5].load(std::memory_order_relaxed), g_byState[6].load(std::memory_order_relaxed),
                 g_rtdTotal.load(std::memory_order_relaxed), g_rtdToInvalid.load(std::memory_order_relaxed),
                 g_fwdTotal.load(std::memory_order_relaxed), g_fwdGarbage.load(std::memory_order_relaxed),
                 g_fwdFromNotFwd.load(std::memory_order_relaxed), g_fwdToNull.load(std::memory_order_relaxed),
                 g_fwdToNotHeap.load(std::memory_order_relaxed), g_fwdToInvalid.load(std::memory_order_relaxed),
                 g_fwdToValid.load(std::memory_order_relaxed), g_fwdLiveZero.load(std::memory_order_relaxed),
                 g_fwdKnownEmpty.load(std::memory_order_relaxed),
                 g_fwdBooksSplit.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace PermWhoAdmit
} // namespace MapleRuntime
