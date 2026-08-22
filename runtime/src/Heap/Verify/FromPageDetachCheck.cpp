// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FromPageDetachCheck.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/RelocationSetTxn.h"

namespace MapleRuntime {
namespace FromPageDetach {
namespace {

struct AtomicCounters {
    std::atomic<uint64_t> checks{ 0 };
    std::atomic<uint64_t> withEvidence{ 0 };
    std::atomic<uint64_t> blocked{ 0 };
    std::atomic<uint64_t> activeTable{ 0 };
    std::atomic<uint64_t> retiredTable{ 0 };
    std::atomic<uint64_t> routeDestHeld{ 0 };
    std::atomic<uint64_t> forwardingPositive{ 0 };
    std::atomic<uint64_t> forwardingReaders{ 0 };
    std::atomic<uint64_t> forwardingClaimed{ 0 };
    std::atomic<uint64_t> forwardingReleased{ 0 };
    std::atomic<uint64_t> copyInflight{ 0 };
    std::atomic<uint64_t> txnEvidence{ 0 };
    std::atomic<uint64_t> txnHandles{ 0 };
    std::atomic<uint64_t> txnOutstanding{ 0 };
};

constexpr size_t kSiteCount = static_cast<size_t>(Site::SITE_COUNT);
std::array<AtomicCounters, kSiteCount> g_counters;
std::atomic<uint64_t> g_quarantineAdmitted{ 0 };
std::atomic<uint64_t> g_quarantineReleased{ 0 };
std::atomic<uint64_t> g_quarantineRecheckHeld{ 0 };
std::atomic<uint64_t> g_quarantinePeakEntries{ 0 };
std::atomic<uint64_t> g_minorRecheckCalls{ 0 };
std::atomic<uint64_t> g_minorPending{ 0 };
std::atomic<uint64_t> g_minorFreed{ 0 };
std::atomic<uint64_t> g_minorHeld{ 0 };
constexpr size_t kBlockBucketCount = 7;
std::array<std::atomic<uint64_t>, kBlockBucketCount> g_heldBlockPages{};
std::array<std::atomic<uint64_t>, kBlockBucketCount> g_heldBlockUnits{};
thread_local uint32_t g_reusePermitDepth = 0;

AtomicCounters& At(Site site)
{
    const size_t i = static_cast<size_t>(site);
    CHECK(i < kSiteCount);
    return g_counters[i];
}

uint64_t Load(const std::atomic<uint64_t>& value)
{
    return value.load(std::memory_order_relaxed);
}

struct DumpAtExit {
    DumpAtExit() { std::atexit(DumpSummary); }
};
const DumpAtExit g_dumpAtExit;

} // namespace

bool GateEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("CJRT_FROM_REUSE_GATE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

ReusePermitScope::ReusePermitScope() { ++g_reusePermitDepth; }

ReusePermitScope::~ReusePermitScope()
{
    CHECK(g_reusePermitDepth > 0);
    --g_reusePermitDepth;
}

const char* SiteName(Site site)
{
    static constexpr const char* kNames[kSiteCount] = {
        "collect_from_garbage",
        "take_garbage",
        "take_after_dispel",
        "reclaim_dirty",
        "reclaim_mark_quarantine",
        "release_region",
        "release_garbage_units",
        "take_garbage_reuse",
        "take_dirty_reuse",
        "take_released_reuse",
        "clear_units",
        "release_units",
        "init_free_units",
        "init_region_info",
        "major_recheck",
        "minor_recheck",
    };
    const size_t i = static_cast<size_t>(site);
    return i < kSiteCount ? kNames[i] : "invalid";
}

bool FromPageDetachCheck(const RegionInfo* region, Site site, Action action)
{
    AtomicCounters& out = At(site);
    out.checks.fetch_add(1, std::memory_order_relaxed);
    if (region == nullptr) {
        return true;
    }

    const MAddress start = region->GetRegionStart();
    const size_t size = region->GetRegionSizeForDetachCheck();
    const bool activeTable = ForwardingTable::GetEntries(start) != nullptr;
    bool retiredTable = ForwardingTable::RetiredCovers(start, size);
    const bool retiredObserved = retiredTable;
    const bool routeDestHeld = region->IsRouteDestHeld();
    const int32_t refCount = region->ForwardingRefCount();
    const bool forwardingPositive = refCount > 0;
    // ref==1 is the relocation worker's construction token, not an external
    // reader. Only the surplus is evidence that detach would still wait.
    const bool forwardingReaders = refCount > 1;
    const bool forwardingClaimed = refCount < 0 || region->ForwardingClaimed();
    // fwdClaimed is a per-life latch. DrainScope leaves it true after publishing
    // ref=0/done=1, and InitRegionInfo resets it only at reuse. Keep the latch in
    // the census, but only a claim that still owns a non-zero ref is evidence.
    const bool forwardingClaimActive = refCount < 0 || (region->ForwardingClaimed() && refCount != 0);
    const bool forwardingReleased = refCount == 0 && region->IsForwardingDone();
    const bool copyInflight = region->CopyInflight() != 0;
    uint64_t txnHandles = 0;
    uint64_t txnOutstanding = 0;
    const bool txnEvidence = RelocationSetTxn::HasDetachEvidence(
        region, region->GetRegionLifeId(), &txnHandles, &txnOutstanding);
    // An active table and its construction token are normal until detach. Keep
    // them visible in the census, but do not call them an unhealed reader. A
    // retired covering table is different: reuse can erase the only remaining
    // answer for a stale slot, which is the i2 two-clock population.
    const bool otherEvidence = routeDestHeld || forwardingReaders || forwardingClaimActive || copyInflight ||
        txnEvidence;
    const bool closeAuthority = GateEnabled() || RelocationSetTxn::Enabled();
    if (action == Action::MAJOR_CLOSE && closeAuthority && retiredTable && !otherEvidence) {
        // zRelocate.cpp:1018-1047: the remap closure is complete. No retained
        // reader or route destination still names this page, so the retired
        // answer has reached its actual grace condition and can be detached.
        ForwardingTable::DropRetiredCovering(start, size, true);
        retiredTable = ForwardingTable::RetiredCovers(start, size);
    }
    const bool any = retiredTable || otherEvidence;

    out.withEvidence.fetch_add(any ? 1 : 0, std::memory_order_relaxed);
    // Phase-2 transactions make their own detach evidence authoritative even
    // when the standalone phase-1 reuse gate remains at its default OFF.
    // All other evidence retains CJRT_FROM_REUSE_GATE's original policy.
    const bool txnAuthority = RelocationSetTxn::Enabled() && txnEvidence;
    const bool blocked = g_reusePermitDepth == 0 && ((GateEnabled() && any) || txnAuthority);
    out.blocked.fetch_add(blocked ? 1 : 0, std::memory_order_relaxed);
    out.activeTable.fetch_add(activeTable ? 1 : 0, std::memory_order_relaxed);
    out.retiredTable.fetch_add(retiredObserved ? 1 : 0, std::memory_order_relaxed);
    out.routeDestHeld.fetch_add(routeDestHeld ? 1 : 0, std::memory_order_relaxed);
    out.forwardingPositive.fetch_add(forwardingPositive ? 1 : 0, std::memory_order_relaxed);
    out.forwardingReaders.fetch_add(forwardingReaders ? 1 : 0, std::memory_order_relaxed);
    out.forwardingClaimed.fetch_add(forwardingClaimed ? 1 : 0, std::memory_order_relaxed);
    out.forwardingReleased.fetch_add(forwardingReleased ? 1 : 0, std::memory_order_relaxed);
    out.copyInflight.fetch_add(copyInflight ? 1 : 0, std::memory_order_relaxed);
    out.txnEvidence.fetch_add(txnEvidence ? 1 : 0, std::memory_order_relaxed);
    out.txnHandles.fetch_add(txnHandles, std::memory_order_relaxed);
    out.txnOutstanding.fetch_add(txnOutstanding, std::memory_order_relaxed);

    return !blocked;
}

Counters GetCounters(Site site)
{
    AtomicCounters& c = At(site);
    return Counters{ Load(c.checks), Load(c.withEvidence), Load(c.blocked), Load(c.activeTable), Load(c.retiredTable),
                     Load(c.routeDestHeld), Load(c.forwardingPositive), Load(c.forwardingReaders),
                     Load(c.forwardingClaimed), Load(c.forwardingReleased), Load(c.copyInflight),
                     Load(c.txnEvidence), Load(c.txnHandles), Load(c.txnOutstanding) };
}

uint32_t ClassifyBlock(const RegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    uint32_t bits = 0;
    const MAddress start = region->GetRegionStart();
    const size_t size = region->GetRegionSizeForDetachCheck();
    if (ForwardingTable::RetiredCovers(start, size)) {
        bits |= static_cast<uint32_t>(BlockBit::RETIRED_TABLE);
    }
    if (region->IsRouteDestHeld()) {
        bits |= static_cast<uint32_t>(BlockBit::ROUTE_DEST_HELD);
    }
    const int32_t refCount = region->ForwardingRefCount();
    if (refCount > 1) {
        bits |= static_cast<uint32_t>(BlockBit::FWD_READERS);
    }
    if (refCount < 0 || (region->ForwardingClaimed() && refCount != 0)) {
        bits |= static_cast<uint32_t>(BlockBit::FWD_CLAIM);
    }
    if (region->CopyInflight() != 0) {
        bits |= static_cast<uint32_t>(BlockBit::COPY_INFLIGHT);
    }
    uint64_t txnHandles = 0;
    uint64_t txnOutstanding = 0;
    if (RelocationSetTxn::HasDetachEvidence(region, region->GetRegionLifeId(), &txnHandles, &txnOutstanding)) {
        if (txnHandles != 0) {
            bits |= static_cast<uint32_t>(BlockBit::TXN_HANDLES);
        }
        if (txnOutstanding != 0) {
            bits |= static_cast<uint32_t>(BlockBit::TXN_NOT_DETACHED);
        }
    }
    return bits;
}

const char* BlockName(uint32_t bits)
{
    if (bits == 0) {
        return "none";
    }
    if (bits == static_cast<uint32_t>(BlockBit::RETIRED_TABLE)) {
        return "retired_table";
    }
    if (bits == static_cast<uint32_t>(BlockBit::ROUTE_DEST_HELD)) {
        return "route_dest_held";
    }
    if (bits == static_cast<uint32_t>(BlockBit::FWD_READERS)) {
        return "fwd_readers";
    }
    if (bits == static_cast<uint32_t>(BlockBit::FWD_CLAIM)) {
        return "fwd_claim";
    }
    if (bits == static_cast<uint32_t>(BlockBit::COPY_INFLIGHT)) {
        return "copy_inflight";
    }
    if (bits == static_cast<uint32_t>(BlockBit::TXN_HANDLES)) {
        return "txn_handles";
    }
    if (bits == static_cast<uint32_t>(BlockBit::TXN_NOT_DETACHED)) {
        return "txn_not_detached";
    }
    return "mixed";
}

void NoteMinorRecheck(size_t pending, size_t freed, size_t held)
{
    g_minorRecheckCalls.fetch_add(1, std::memory_order_relaxed);
    g_minorPending.fetch_add(pending, std::memory_order_relaxed);
    g_minorFreed.fetch_add(freed, std::memory_order_relaxed);
    g_minorHeld.fetch_add(held, std::memory_order_relaxed);
}

void NoteHeldBlock(uint32_t bits, size_t units)
{
    size_t idx = kBlockBucketCount - 1;
    if (bits == static_cast<uint32_t>(BlockBit::RETIRED_TABLE)) {
        idx = 0;
    } else if (bits == static_cast<uint32_t>(BlockBit::ROUTE_DEST_HELD)) {
        idx = 1;
    } else if (bits == static_cast<uint32_t>(BlockBit::FWD_READERS)) {
        idx = 2;
    } else if (bits == static_cast<uint32_t>(BlockBit::FWD_CLAIM)) {
        idx = 3;
    } else if (bits == static_cast<uint32_t>(BlockBit::COPY_INFLIGHT)) {
        idx = 4;
    } else if (bits == static_cast<uint32_t>(BlockBit::TXN_HANDLES) ||
               bits == static_cast<uint32_t>(BlockBit::TXN_NOT_DETACHED) ||
               bits == (static_cast<uint32_t>(BlockBit::TXN_HANDLES) |
                        static_cast<uint32_t>(BlockBit::TXN_NOT_DETACHED))) {
        idx = 5;
    }
    g_heldBlockPages[idx].fetch_add(1, std::memory_order_relaxed);
    g_heldBlockUnits[idx].fetch_add(units, std::memory_order_relaxed);
}

void DumpHeldBuckets(const char* why)
{
    static const char* kBucket[] = { "retired_table", "route_dest_held", "fwd_readers", "fwd_claim",
                                     "copy_inflight", "txn", "mixed" };
    std::fprintf(stderr, "[GCV2][detach-hold] why=%s", why == nullptr ? "?" : why);
    for (size_t i = 0; i < kBlockBucketCount; ++i) {
        std::fprintf(stderr, " %s_pages=%llu_units=%llu", kBucket[i],
                     static_cast<unsigned long long>(Load(g_heldBlockPages[i])),
                     static_cast<unsigned long long>(Load(g_heldBlockUnits[i])));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

QuarantineCounters GetQuarantineCounters()
{
    return QuarantineCounters{ Load(g_quarantineAdmitted), Load(g_quarantineReleased),
                               Load(g_quarantineRecheckHeld), Load(g_quarantinePeakEntries),
                               Load(g_minorRecheckCalls), Load(g_minorPending), Load(g_minorFreed),
                               Load(g_minorHeld) };
}

void NoteQuarantineAdmitted(uint64_t entriesNow)
{
    g_quarantineAdmitted.fetch_add(1, std::memory_order_relaxed);
    uint64_t peak = g_quarantinePeakEntries.load(std::memory_order_relaxed);
    while (entriesNow > peak &&
           !g_quarantinePeakEntries.compare_exchange_weak(peak, entriesNow, std::memory_order_relaxed)) {
    }
}

void NoteQuarantineReleased() { g_quarantineReleased.fetch_add(1, std::memory_order_relaxed); }

void NoteQuarantineRecheckHeld() { g_quarantineRecheckHeld.fetch_add(1, std::memory_order_relaxed); }

void DumpSummary()
{
    for (size_t i = 0; i < kSiteCount; ++i) {
        const Site site = static_cast<Site>(i);
        const Counters c = GetCounters(site);
        std::fprintf(stderr,
                     "[GCV2][detach-check] phase=%s site=%s checks=%llu evidence=%llu blocked=%llu "
                     "active_table=%llu retired_table=%llu route_dest_held=%llu fwd_positive=%llu "
                     "fwd_readers=%llu fwd_claimed=%llu fwd_released=%llu copy_inflight=%llu "
                     "txn_evidence=%llu txn_handles=%llu txn_outstanding=%llu\n",
                     GateEnabled() ? "enforce" : "measure", SiteName(site),
                     static_cast<unsigned long long>(c.checks),
                     static_cast<unsigned long long>(c.withEvidence),
                     static_cast<unsigned long long>(c.blocked),
                     static_cast<unsigned long long>(c.activeTable),
                     static_cast<unsigned long long>(c.retiredTable),
                     static_cast<unsigned long long>(c.routeDestHeld),
                     static_cast<unsigned long long>(c.forwardingPositive),
                     static_cast<unsigned long long>(c.forwardingReaders),
                     static_cast<unsigned long long>(c.forwardingClaimed),
                     static_cast<unsigned long long>(c.forwardingReleased),
                     static_cast<unsigned long long>(c.copyInflight),
                     static_cast<unsigned long long>(c.txnEvidence),
                     static_cast<unsigned long long>(c.txnHandles),
                     static_cast<unsigned long long>(c.txnOutstanding));
    }
    const QuarantineCounters q = GetQuarantineCounters();
    std::fprintf(stderr,
                 "[GCV2][detach-quarantine] gate=%u admitted=%llu released=%llu recheck_held=%llu "
                 "peak_entries=%llu max_entries=65536 max_rechecks=8 "
                 "minor_recheck_calls=%llu minor_pending=%llu minor_freed=%llu minor_held=%llu\n",
                 static_cast<unsigned>(GateEnabled()), static_cast<unsigned long long>(q.admitted),
                 static_cast<unsigned long long>(q.released), static_cast<unsigned long long>(q.recheckHeld),
                 static_cast<unsigned long long>(q.peakEntries),
                 static_cast<unsigned long long>(q.minorRecheckCalls),
                 static_cast<unsigned long long>(q.minorPending),
                 static_cast<unsigned long long>(q.minorFreed),
                 static_cast<unsigned long long>(q.minorHeld));
    DumpHeldBuckets("atexit");
    std::fflush(stderr);
}

} // namespace FromPageDetach
} // namespace MapleRuntime
