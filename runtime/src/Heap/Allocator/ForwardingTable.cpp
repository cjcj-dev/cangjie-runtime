// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/ForwardingTable.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/ZGranuleMap.h"
#include "Heap/Verify/M0Correlation.h"
#include "Heap/WCollector/WCollector.h"

namespace MapleRuntime {
namespace {

// zForwardingTable.hpp:32-52 — one granule map of ZForwarding*.
// Two maps because Dispel unlinks membership while ClearEntries (a phase later)
// is when the attached array may be retired. ZGC has one map: reset_relocation_set
// is the only unlink (zGeneration.cpp:276-285).
ZGranuleMap<ZForwarding*> g_membership;
ZGranuleMap<ZForwarding*> g_entries;
// Per-region-span generation word: bit 0 is open, upper bits are the
// monotonically increasing generation. Clear leaves the closed tombstone in
// place, so a missing g_entries pointer can never be mistaken for permission to
// allocate a replacement.
ZGranuleMap<uint64_t> g_publicationState;
constexpr uint64_t kPublicationOpen = 1;

// Product boundary: callers still own virtual addresses, while ZGranuleMap
// consumes only heap offsets. These helpers force every consumer through the
// map's checked MAddress -> zoffset gate before an array element can be formed.
ZForwarding* MapGet(const ZGranuleMap<ZForwarding*>& map, MAddress addr)
{
    zoffset offset;
    return map.offset_for_address(addr, &offset) ? map.get(offset) : nullptr;
}

void MapPut(ZGranuleMap<ZForwarding*>& map, MAddress addr, size_t size, ZForwarding* value)
{
    zoffset offset;
    if (map.offset_for_address(addr, &offset)) {
        map.put(offset, size, value);
    }
}

ZForwarding* MapExchange(ZGranuleMap<ZForwarding*>& map, MAddress addr, ZForwarding* value)
{
    zoffset offset;
    return map.offset_for_address(addr, &offset) ? map.exchange(offset, value) : nullptr;
}

uint64_t PublicationStateAt(MAddress addr)
{
    zoffset offset;
    return g_publicationState.offset_for_address(addr, &offset) ? g_publicationState.get(offset) : 0;
}

void PutPublicationState(MAddress addr, size_t size, uint64_t state)
{
    zoffset offset;
    if (g_publicationState.offset_for_address(addr, &offset)) {
        g_publicationState.put(offset, size, state);
    }
}

uint64_t PublicationGeneration(uint64_t state) { return state >> 1; }

bool PublicationOpen(uint64_t state) { return (state & kPublicationOpen) != 0; }

void SealPublicationLocked(MAddress start, size_t size)
{
    PutPublicationState(start, size, PublicationStateAt(start) & ~kPublicationOpen);
}

std::atomic<bool> g_ready{ false };
// Installation is rare and phase-scoped. Serialize provisional-to-full replacement so
// readers never observe a freed membership carrier while the attached array is resized.
std::mutex g_installLock;

std::atomic<uint64_t> g_cmpTotal{ 0 };
std::atomic<uint64_t> g_cmpAgree{ 0 };
std::atomic<uint64_t> g_cmpTableOnly{ 0 };
std::atomic<uint64_t> g_cmpLegacyOnly{ 0 };
constexpr unsigned kTypeBuckets = 16;
std::atomic<uint64_t> g_tableOnlyByType[kTypeBuckets] = {};
std::atomic<uint64_t> g_legacyOnlyByType[kTypeBuckets] = {};

std::atomic<uint64_t> g_destTotal{ 0 };
std::atomic<uint64_t> g_destAgree{ 0 };
std::atomic<uint64_t> g_destDisagree{ 0 };
std::atomic<uint64_t> g_destPending{ 0 };
std::atomic<uint64_t> g_destDisagreeByType[kTypeBuckets] = {};
std::atomic<uint64_t> g_armedHit{ 0 };
std::atomic<uint64_t> g_armedMiss{ 0 };
std::atomic<uint64_t> g_unavailable{ 0 };
std::atomic<uint64_t> g_unarmed{ 0 };
std::atomic<uint64_t> g_markCoverageEpoch[2] = { { 0 }, { 0 } };

void StampTableCoverage(ZForwarding* tab, RegionInfo* region)
{
    if (tab == nullptr) {
        return;
    }
    const Generation gen = region == nullptr ? Generation::Young : region->GetOwnerGeneration();
    const size_t idx = static_cast<size_t>(gen);
    const uint64_t birth = g_markCoverageEpoch[idx].load(std::memory_order_acquire);
    tab->note_table_epoch(static_cast<uint8_t>(gen),
                         WCollector::FlipSeq().load(std::memory_order_relaxed), birth + 1);
}

#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<ForwardingTable::LookupRetainHook> g_lookupRetainHook{ nullptr };
std::atomic<void*> g_lookupRetainHookContext{ nullptr };
std::atomic<ForwardingTable::ReceiptLifeRegisterHook> g_receiptLifeRegisterHook{ nullptr };
std::atomic<void*> g_receiptLifeRegisterHookContext{ nullptr };
#endif

bool PublicationClosedAt(MAddress addr)
{
    zoffset offset;
    if (!g_publicationState.offset_for_address(addr, &offset)) {
        return false;
    }
    return !PublicationOpen(g_publicationState.get(offset));
}

} // namespace

bool ForwardingTable::Ready() { return g_ready.load(std::memory_order_acquire); }

bool ForwardingTable::Initialize(MAddress heapStart, size_t heapSize, size_t unitSize)
{
    if (g_ready.load(std::memory_order_acquire)) {
#if defined(MRT_GC_UNIT_TESTS)
        if (g_entries.base() != heapStart) {
            // Aggregate gc_unit creates a fresh synthetic mmap per test. Each
            // fixture destructor has already drained/reclaimed its carriers;
            // rebase only the test build so the next fixture exercises the
            // same product address checks rather than an obsolete map base.
            g_membership.ResetForTest();
            g_entries.ResetForTest();
            g_publicationState.ResetForTest();
            g_ready.store(false, std::memory_order_release);
        } else {
            return true;
        }
#else
        return true;
#endif
    }
    if (unitSize == 0 || heapSize == 0) {
        return false;
    }
    if (!g_membership.Initialize(heapStart, heapSize, unitSize) ||
        !g_entries.Initialize(heapStart, heapSize, unitSize) ||
        !g_publicationState.Initialize(heapStart, heapSize, unitSize)) {
        LOG(RTLOG_ERROR, "[FWDTABLE] granule map init failed size=%zu unit=%zu -- table stays off", heapSize,
            unitSize);
        return false;
    }
    // Generation zero is the initial, pre-cycle provisional-membership epoch.
    // The first ClearEntries seals it exactly like every later generation;
    // only PreparePublicationGeneration may open the next one.
    PutPublicationState(heapStart, heapSize, kPublicationOpen);
    g_ready.store(true, std::memory_order_release);
    LOG(RTLOG_ERROR, "[FWDTABLE] armed base=%#zx size=%zu unit=%zu entries=%zu", static_cast<size_t>(heapStart),
        heapSize, unitSize, g_membership.size());
    static std::atomic<bool> dumped{ false };
    bool expected = false;
    if (dumped.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[FWDTABLE][refuse] atexit full=%llu overflow=%llu fallbackFull=%llu "
                         "fallbackOverflow=%llu armedHit=%llu armedMiss=%llu unavailable=%llu unarmed=%llu\n",
                         static_cast<unsigned long long>(ZForwarding::FullRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::OverflowRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::FullFallbacks().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::OverflowFallbacks().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(ForwardingTable::ArmedHitCount()),
                         static_cast<unsigned long long>(ForwardingTable::ArmedMissCount()),
                         static_cast<unsigned long long>(ForwardingTable::UnavailableCount()),
                         static_cast<unsigned long long>(ForwardingTable::UnarmedCount()));
        });
    }
    return true;
}

uint32_t ForwardingTable::EstimateLiveObjects(RegionInfo* region, size_t regionSize)
{
    // zForwarding.inline.hpp:43-50 sizes from live *object* count. GetLiveByteCount
    // is bytes; liveBytes>>3 counts 8-byte words. Before marking has made zero
    // authoritative, take the region's capacity so the table cannot fill and spin
    // (REPORT-fwdentries). A closed zero-live face needs only the minimum table.
    const uint64_t liveBytes = region->GetLiveByteCount();
    uint64_t estimate = liveBytes >> ZForwarding::kAlignShift;
    if (estimate == 0 && !region->IsLiveCountAuthoritative()) {
        estimate = regionSize >> ZForwarding::kAlignShift;
    }
    if (estimate == 0) {
        estimate = 1;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(estimate, UINT32_MAX));
}

void ForwardingTable::insert(ZForwarding* forwarding)
{
    // zForwardingTable.inline.hpp:48-54
    if (forwarding == nullptr || !Ready()) {
        return;
    }
    MapPut(g_membership, forwarding->start(), forwarding->size(), forwarding);
    MapPut(g_entries, forwarding->start(), forwarding->size(), forwarding);
}

void ForwardingTable::remove(ZForwarding* forwarding)
{
    // zForwardingTable.inline.hpp:56-62 — membership only. Entries stay until
    // ClearEntries (zRelocationSet.cpp:91-96 arena recycle is a phase later).
    if (forwarding == nullptr || !Ready()) {
        return;
    }
    MapPut(g_membership, forwarding->start(), forwarding->size(), nullptr);
}

ZForwarding* ForwardingTable::get(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    ZForwarding* forwarding = MapGet(g_membership, addr);
    if (forwarding != nullptr && !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
        return nullptr;
    }
    return forwarding;
}

bool ForwardingTable::PreparePublicationGeneration(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    const uint64_t state = PublicationStateAt(regionStart);
    if (PublicationOpen(state)) {
        return false;
    }
    const uint64_t previous = PublicationGeneration(state);
    CHECK_DETAIL(previous < (UINT64_MAX >> 1),
                 "forwarding publication generation exhausted start=%#zx", static_cast<size_t>(regionStart));
    PutPublicationState(regionStart, regionSize, ((previous + 1) << 1) | kPublicationOpen);
    return true;
}

bool ForwardingTable::InstallPublicationBeforeCopy(
    MAddress regionStart, size_t regionSize, RegionInfo* region)
{
    if (!Ready() || regionSize == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* forwarding = EnsureEntriesLocked(region);
    if (forwarding == nullptr || forwarding->start() != regionStart || forwarding->size() < regionSize ||
        forwarding->is_provisional()) {
        return false;
    }
    MapPut(g_membership, regionStart, regionSize, forwarding);
    return true;
}

bool ForwardingTable::InsertProvisional(MAddress regionStart, size_t regionSize, RegionInfo* region)
{
    if (!Ready() || region == nullptr || regionSize == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    const uint64_t publicationState = PublicationStateAt(regionStart);
    // Provisional membership is still an installation. Once ClearEntries seals
    // this generation, it must wait for the next explicit prepare boundary.
    if (!PublicationOpen(publicationState)) {
        return false;
    }
    ZForwarding* forwarding = MapGet(g_entries, regionStart);
    const uint64_t generation = PublicationGeneration(publicationState);
    const bool usable = forwarding != nullptr && forwarding->start() == regionStart &&
        forwarding->size() >= regionSize && forwarding->page() == region &&
        forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY) &&
        forwarding->publication_generation() == generation;
    if (forwarding != nullptr && !usable) {
        // Replacing any carrier under an open generation would create two table
        // identities for one cycle. Seal, drain, and require a new prepare.
        SealPublicationLocked(regionStart, regionSize);
        if (!forwarding->claim()) {
            forwarding->detach_page();
        } else {
            forwarding->in_place_relocation_claim_page();
            forwarding->mark_done();
            forwarding->release_page();
        }
        MapPut(g_entries, regionStart, regionSize, nullptr);
        if (MapGet(g_membership, regionStart) == forwarding) {
            MapPut(g_membership, regionStart, regionSize, nullptr);
        }
        Retire(forwarding);
        return false;
    }
    if (forwarding == nullptr) {
        // Two entries preserve the existing armed-miss semantics and membership publication,
        // without zeroing a capacity-sized table before marking has established live bytes.
        ZForwarding* created = ZForwarding::alloc(1, regionStart, g_entries.base(), regionSize, region,
                                                 region->GetRegionLifeId(), true);
        if (created == nullptr) {
            return false;
        }
        created->set_publication_generation(generation);
        StampTableCoverage(created, region);
        RegionLifeClock::Publish(RegionLifeClock::Carrier::ARMED_ENTRY, created->page_life_id());
        MapPut(g_entries, regionStart, regionSize, created);
        forwarding = created;
    }
    MapPut(g_membership, regionStart, regionSize, forwarding);
    return true;
}

void ForwardingTable::Remove(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    MapPut(g_membership, regionStart, regionSize, nullptr);
}

namespace {
void DrainPublicationOwners(ZForwarding* forwarding);
} // namespace

void ForwardingTable::RetireMembershipAtDispel(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    // Seal before unlinking, the same order ClearEntries uses: a would-be publisher that loses
    // this lock then observes the tombstone instead of an open generation. DispelGhostFromRegion
    // runs after DrainScope has refused late retainers and waited out existing readers, so no
    // Publication owner can still need this generation open (zForwarding.cpp:171-181).
    SealPublicationLocked(regionStart, regionSize);
    // One unlink edge, the way ZGC has one: ZRelocationSet::reset destroys every ZForwarding the
    // finished set owned (zRelocationSet.cpp:191-197), ZForwardingTable::get answers only for a
    // page still in the live set (zForwardingTable.inline.hpp:36-46), and a ZForwarding detaches
    // when the relocation that created it ends (zForwarding.cpp:171-181).  Keeping the entry map
    // linked until a later free left the finished cycle's receipts queryable, and under in-place
    // compaction a finished cycle's destinations are live addresses of the next cycle: querying
    // them a second time returns the destination the *same* shift already produced, one shift
    // further down the page.  Unlink both maps here so the finished set can answer nothing.
    ZForwarding* tab = MapGet(g_entries, regionStart);
    if (tab != nullptr && tab->start() == regionStart) {
        DrainPublicationOwners(tab);
    }
    (void)MapExchange(g_entries, regionStart, nullptr);
    MapPut(g_entries, regionStart, regionSize, nullptr);
    MapPut(g_membership, regionStart, regionSize, nullptr);
    if (tab != nullptr && tab->start() == regionStart) {
        Retire(tab);
    }
}

namespace {
void DrainPublicationOwners(ZForwarding* forwarding)
{
    if (forwarding == nullptr) {
        return;
    }
    if (!forwarding->claim()) {
        forwarding->detach_page();
        return;
    }
    forwarding->in_place_relocation_claim_page();
    forwarding->mark_done();
    forwarding->release_page();
    CHECK_DETAIL(forwarding->ref_count().load(std::memory_order_acquire) == 0,
                 "forwarding responsibility not drained tab=%p start=%#zx ref=%d", forwarding,
                 static_cast<size_t>(forwarding->start()),
                 forwarding->ref_count().load(std::memory_order_relaxed));
}
} // namespace

ZForwarding* ForwardingTable::EnsureEntriesLocked(RegionInfo* region)
{
    if (!Ready() || region == nullptr) {
        return nullptr;
    }
    const MAddress start = region->GetRegionStart();
    const size_t regionSize = region->GetRegionSize();
    zoffset startOffset;
    if (!g_entries.offset_for_address(start, &startOffset)) {
        return nullptr;
    }
    const uint64_t publicationState = PublicationStateAt(start);
    if (!PublicationOpen(publicationState)) {
        return nullptr;
    }
    const uint64_t generation = PublicationGeneration(publicationState);
    ZForwarding* previous = g_entries.get(startOffset);
    if (previous != nullptr && !previous->is_provisional() && previous->start() == start &&
        previous->size() >= regionSize && previous->page() == region &&
        previous->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY) &&
        previous->publication_generation() == generation) {
        return previous;
    }
    // A malformed table from this same open generation is not replaceable: a
    // copier could already carry its identity. Seal it and require the next
    // explicit PreparePublicationGeneration instead of silently installing a
    // second identity under late consumers.
    if (previous != nullptr && previous->publication_generation() == generation) {
        SealPublicationLocked(start, regionSize);
        DrainPublicationOwners(previous);
        g_entries.put(startOffset, regionSize, nullptr);
        if (MapGet(g_membership, start) == previous) {
            MapPut(g_membership, start, regionSize, nullptr);
        }
        Retire(previous);
        return nullptr;
    }
    const uint32_t liveObjs = EstimateLiveObjects(region, regionSize);
    const RegionLifeId life = region->GetRegionLifeId();
    ZForwarding* created = ZForwarding::alloc(liveObjs, start, g_entries.base(), regionSize, region, life);
    if (created == nullptr) {
        SealPublicationLocked(start, regionSize);
        return nullptr;
    }
    created->set_publication_generation(generation);
    StampTableCoverage(created, region);
    RegionLifeClock::Publish(RegionLifeClock::Carrier::ARMED_ENTRY, created->page_life_id());
    // Keep the previous table mapped until every copier carrying it has
    // inserted its receipt. g_installLock prevents a new acquisition while the
    // old generation drains.
    DrainPublicationOwners(previous);
    g_entries.put(startOffset, regionSize, created);
    if (previous != nullptr && MapGet(g_membership, start) == previous) {
        MapPut(g_membership, start, regionSize, created);
    }
    if (previous != nullptr) {
        Retire(previous);
    }
    return created;
}

void ForwardingTable::ClearEntries(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    // The tombstone is published before claim/drain. A late before-copy or
    // after-copy acquisition that loses this lock therefore fails closed even
    // after g_entries has been unlinked.
    SealPublicationLocked(regionStart, regionSize);
    ZForwarding* tab = MapGet(g_entries, regionStart);
    if (tab != nullptr && tab->start() == regionStart) {
        // Seal while the table is still mapped. Existing Publication owners can
        // finish insert/publish; new owners cannot pass g_installLock. Only then
        // may reset unlink the forwarding (zGeneration.cpp:276-284).
        DrainPublicationOwners(tab);
    }
    (void)MapExchange(g_entries, regionStart, nullptr);
    MapPut(g_entries, regionStart, regionSize, nullptr);
    if (tab != nullptr && tab->start() == regionStart) {
        Retire(tab);
    }
}

static void UnlinkThenDestroy(ZForwarding* tab)
{
    if (tab == nullptr) {
        return;
    }
    // Membership and entries may still name a retired object (ExpireKept
    // ClearEntries parks it; get() must stay valid until the object is
    // actually freed). Null both before Destroy so get() cannot UAF
    // (zForwardingTable.inline.hpp:56-62 remove, then arena recycle).
    if (MapGet(g_membership, tab->start()) == tab) {
        MapPut(g_membership, tab->start(), tab->size(), nullptr);
    }
    if (MapGet(g_entries, tab->start()) == tab) {
        MapPut(g_entries, tab->start(), tab->size(), nullptr);
    }
    tab->Destroy();
}

namespace {
std::mutex g_retiredLock;
std::vector<ZForwarding*> g_retired;
// Previous reset generation. ZGeneration::reset_relocation_set destroys the
// set installed last cycle, after this cycle's mark (zGeneration.cpp:276-285).
std::vector<ZForwarding*> g_retiredPrev;
std::atomic<uint64_t> g_retiredTotal{ 0 };
std::atomic<uint64_t> g_reclaimedTotal{ 0 };
} // namespace

static bool ReclaimWhyForceCoverageComplete(const char* why)
{
    if (why == nullptr) {
        return false;
    }
    if (std::strcmp(why, "gc-unit-fixture-coverage-complete") == 0 ||
        std::strcmp(why, "gc-unit-explicit-coverage") == 0 ||
        std::strstr(why, "cleanup") != nullptr) {
        return true;
    }
    return false;
}

static bool CoverageEpochSatisfied(ZForwarding* tab)
{
    if (tab == nullptr) {
        return false;
    }
    const size_t idx = static_cast<size_t>(tab->table_generation());
    if (idx > 1) {
        return false;
    }
    return g_markCoverageEpoch[idx].load(std::memory_order_acquire) >= tab->required_mark_epoch();
}

bool ForwardingTable::RetiredDestroyEligible(ZForwarding* tab)
{
    if (tab == nullptr) {
        return false;
    }
    if (!CoverageEpochSatisfied(tab)) {
        return false;
    }
    const int32_t refs = tab->ref_count().load(std::memory_order_acquire);
    return refs == 0 || refs == 1;
}

void ForwardingTable::Retire(ZForwarding* tab)
{
    if (tab == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_retiredLock);
    ZForwardingLife::ResetForForwarding(tab->ref_count(), tab->claimed(), tab->done());
    g_retired.push_back(tab);
    RegionLifeClock::Publish(RegionLifeClock::Carrier::RETIRED_ENTRY, tab->page_life_id());
    g_retiredTotal.fetch_add(1, std::memory_order_relaxed);
}

void ForwardingTable::ReclaimRetired(const char* why)
{
    std::vector<ZForwarding*> candidates;
    std::vector<ZForwarding*> deferred;
    std::vector<ZForwarding*> victims;
    size_t stillHeld = 0;
    {
        // ClearEntries/EnsureEntries take install before Retire takes retired;
        // keep that lock order. Retired lookups block here instead of observing
        // a transient empty vector while a required carrier is classified.
        std::lock_guard<std::mutex> installLock(g_installLock);
        std::lock_guard<std::mutex> retiredLock(g_retiredLock);
        candidates = g_retired;
        for (ZForwarding* tab : g_retiredPrev) {
            candidates.push_back(tab);
        }
        for (ZForwarding* tab : candidates) {
            RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::RETIRED_ENTRY,
                                                    tab != nullptr,
                                                    tab == nullptr ? 0 : tab->page_life_id());
        }
        const bool forceCoverageComplete = ReclaimWhyForceCoverageComplete(why);
        g_retired.clear();
        g_retiredPrev.clear();
        for (ZForwarding* tab : candidates) {
            if (tab == nullptr) {
                continue;
            }
            if (forceCoverageComplete || RetiredDestroyEligible(tab)) {
                victims.push_back(tab);
            } else {
                deferred.push_back(tab);
            }
        }
        g_retired = deferred;
        stillHeld = g_retired.size();
    }
    for (ZForwarding* tab : victims) {
        UnlinkThenDestroy(tab);
    }
    if (!candidates.empty()) {
        const uint64_t done = victims.empty()
            ? g_reclaimedTotal.load(std::memory_order_relaxed)
            : g_reclaimedTotal.fetch_add(victims.size(), std::memory_order_relaxed) + victims.size();
        LOG(RTLOG_ERROR,
            "[FWDTABLE][reclaim] why=%s freed=%zu deferred=%zu coverage_complete=%u "
            "retired_total=%lu reclaimed_total=%lu",
            why == nullptr ? "?" : why, victims.size(), stillHeld, stillHeld == 0 ? 1u : 0u,
            g_retiredTotal.load(std::memory_order_relaxed), done);
    }
}

void ForwardingTable::PublishMarkCoverage(Generation gen)
{
    const size_t idx = static_cast<size_t>(gen);
    if (idx > 1) {
        return;
    }
    g_markCoverageEpoch[idx].fetch_add(1, std::memory_order_acq_rel);
}

uint64_t ForwardingTable::MarkCoverageEpoch(Generation gen)
{
    const size_t idx = static_cast<size_t>(gen);
    if (idx > 1) {
        return 0;
    }
    return g_markCoverageEpoch[idx].load(std::memory_order_acquire);
}

size_t ForwardingTable::RetiredQueueSize()
{
    std::lock_guard<std::mutex> lock(g_retiredLock);
    return g_retired.size() + g_retiredPrev.size();
}

ForwardingTable::Publication ForwardingTable::RetainCovering(MAddress from)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* tab = GetCovering(from);
    if (tab == nullptr || !tab->retain_page()) {
        return Publication();
    }
    return Publication(tab);
}

bool ForwardingTable::RetiredCovers(MAddress regionStart, size_t regionSize)
{
    if (regionSize == 0) {
        return false;
    }
    const MAddress regionEnd = regionStart + regionSize;
    std::lock_guard<std::mutex> lock(g_retiredLock);
    auto covers = [regionStart, regionEnd](const std::vector<ZForwarding*>& tables) {
        for (ZForwarding* tab : tables) {
            if (tab == nullptr) {
                continue;
            }
            const MAddress tabStart = tab->start();
            const MAddress tabEnd = tabStart + tab->size();
            if (tabStart < regionEnd && regionStart < tabEnd) {
                return true;
            }
        }
        return false;
    };
    return covers(g_retired) || covers(g_retiredPrev);
}

bool ForwardingTable::HasLiveCarrier(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return false;
    }
    if (GetEntries(regionStart) != nullptr) {
        return true;
    }
    return RetiredCovers(regionStart, regionSize);
}

ZForwarding* ForwardingTable::GetEntries(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    ZForwarding* forwarding = MapGet(g_entries, addr);
    if (forwarding != nullptr && !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
        return nullptr;
    }
    return forwarding;
}

ZForwarding* ForwardingTable::GetCovering(MAddress addr)
{
    ZForwarding* armed = GetEntries(addr);
    if (armed != nullptr) {
        return armed;
    }
    ZForwarding* member = Get(addr);
    if (member != nullptr) {
        return member;
    }
    std::lock_guard<std::mutex> lock(g_retiredLock);
    auto scan = [&](const std::vector<ZForwarding*>& gens) -> ZForwarding* {
        for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
            ZForwarding* tab = *it;
            if (tab != nullptr && tab->covers(addr)) {
                return tab;
            }
        }
        return nullptr;
    };
    if (ZForwarding* tab = scan(g_retired)) {
        return tab;
    }
    return scan(g_retiredPrev);
}

bool ForwardingTable::PublishFromPageView(RegionInfo* region, LiveInfo* liveInfo, uint64_t epoch,
                                          MAddress topAtStart, MAddress markStartAllocPtr,
                                          uint64_t liveByteCount, uint8_t owner,
                                          uint8_t largeMarked, RegionLifeId lifeId)
{
    if (region == nullptr || lifeId == 0 || region->GetRegionLifeId() != lifeId) {
        return false;
    }
    ZForwarding* carrier = GetEntries(region->GetRegionStart());
    if (carrier == nullptr || carrier->page() != region) {
        return false;
    }
    carrier->publish_from_page_view(liveInfo, epoch, topAtStart, markStartAllocPtr,
                                    liveByteCount, owner, largeMarked, lifeId);
    RegionLifeClock::Publish(RegionLifeClock::Carrier::MARK_SNAPSHOT, lifeId);
    return true;
}

const ZForwarding::FromPageView* ForwardingTable::GetFromPageView(RegionInfo* region)
{
    if (region == nullptr) {
        return nullptr;
    }
    ZForwarding* carrier = GetEntries(region->GetRegionStart());
    if (carrier == nullptr || carrier->page() != region) {
        return nullptr;
    }
    return carrier->from_page_view(region->GetRegionLifeId());
}

bool ZForwarding::page_life_current(RegionLifeClock::Carrier carrier) const
{
    if (_page == nullptr) {
        RegionLifeClock::NoteUntracked(carrier);
        return true;
    }
    return RegionLifeClock::Validate(carrier, _page_life_id, _page->GetRegionLifeId());
}

ForwardingTable::Publication ForwardingTable::EnsurePublicationBeforeCopy(
    RegionInfo* region, MAddress from)
{
    if (!Ready() || region == nullptr) {
        return Publication();
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* tab = EnsureEntriesLocked(region);
    const uint64_t state = PublicationStateAt(region->GetRegionStart());
    if (tab == nullptr || tab->is_provisional() || tab->start() != region->GetRegionStart() ||
        tab->size() < region->GetRegionSize() || !tab->covers(from) || !PublicationOpen(state) ||
        tab->publication_generation() != PublicationGeneration(state) || !tab->retain_page()) {
        return Publication();
    }
    return Publication(tab);
}

ForwardingTable::Publication ForwardingTable::RetainOpenPublicationAfterCopy(
    RegionInfo* region, MAddress from)
{
    if (!Ready() || region == nullptr) {
        return Publication();
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    const uint64_t state = PublicationStateAt(region->GetRegionStart());
    if (!PublicationOpen(state)) {
        return Publication();
    }
    ZForwarding* tab = MapGet(g_entries, from);
    if (tab == nullptr || tab->is_provisional() || tab->start() != region->GetRegionStart() ||
        tab->size() < region->GetRegionSize() || !tab->covers(from) ||
        tab->publication_generation() != PublicationGeneration(state) || !tab->retain_page()) {
        return Publication();
    }
    return Publication(tab);
}

ZForwarding::Receipt ForwardingTable::InstallMapping(
    const Publication& publication, MAddress from, MAddress to)
{
    ZForwarding* tab = publication.forwarding;
    CHECK_DETAIL(tab != nullptr && !tab->is_provisional() && tab->covers(from),
                 "forwarding publication responsibility missing from=%#zx to=%#zx tab=%p",
                 static_cast<size_t>(from), static_cast<size_t>(to), tab);
    const ZForwarding::Receipt receipt = tab->install_receipt_with_life(from, to, []() {
#if defined(MRT_TESTABLE_INTERNALS)
        ForwardingTable::ReceiptLifeRegisterHook hook =
            g_receiptLifeRegisterHook.load(std::memory_order_acquire);
        if (hook != nullptr) {
            hook(g_receiptLifeRegisterHookContext.load(std::memory_order_acquire));
        }
#endif
    });
    if (receipt.address != 0) {
        // Compact/kept/in-place/promote/unmovable/ForwardRegion all publish here.
        // ReclaimRetired must not unlink a table that still has queryable receipts
        // (zRelocationSet.cpp:191-200; zForwarding.cpp:134-180).
        tab->note_retired_required();
    }
    M0Correlation::PropagateForwarding(from, receipt.address, receipt.address, receipt.installed);
    return receipt;
}

MAddress ForwardingTable::InsertMapping(const Publication& publication, MAddress from, MAddress to)
{
    return InstallMapping(publication, from, to).address;
}

void ForwardingTable::Publication::Release()
{
    if (forwarding != nullptr) {
        forwarding->release_page();
        forwarding = nullptr;
    }
}

ForwardingTable::Publication::~Publication() { Release(); }

ForwardingTable::Publication::Publication(Publication&& other) noexcept
    : forwarding(std::exchange(other.forwarding, nullptr))
{
}

ForwardingTable::Publication& ForwardingTable::Publication::operator=(Publication&& other) noexcept
{
    if (this != &other) {
        Release();
        forwarding = std::exchange(other.forwarding, nullptr);
    }
    return *this;
}

bool ForwardingTable::ReceiptAllowsForwarded(MAddress mapped)
{
    return mapped != 0;
}

std::atomic<uint64_t>& ZForwarding::StaleToLifeCount()
{
    static std::atomic<uint64_t> n{ 0 };
    return n;
}

uint64_t ForwardingTable::StaleToLifeCount()
{
    return ZForwarding::StaleToLifeCount().load(std::memory_order_relaxed);
}

void ZForwarding::note_to_life(MAddress to)
{
    const uint8_t optimisticCount = _to_life_n.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(_receiptInstallLock);
    (void)register_to_life_locked(to, optimisticCount);
}

ZForwarding::Receipt::Status ZForwarding::register_to_life_locked(MAddress to, uint8_t optimisticCount)
{
    (void)optimisticCount;
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr) {
        return Receipt::Status::DESTINATION_UNTRACKED;
    }
    const MAddress start = toRegion->GetRegionStart();
    const uint8_t seq = toRegion->GetRegionLifeSeq();
    const RegionLifeId life = toRegion->GetRegionLifeId();
    if (life == 0) {
        return Receipt::Status::DESTINATION_UNTRACKED;
    }
    const uint8_t count = _to_life_n.load(std::memory_order_relaxed);
    for (uint8_t i = 0; i < count; ++i) {
        if (_to_lives[i].start == start) {
            return _to_lives[i].lifeId == life && _to_lives[i].legacySeq == seq
                ? Receipt::Status::EXISTING
                : Receipt::Status::DESTINATION_LIFE_CONFLICT;
        }
    }
    if (count >= kToLifeCapacity) {
        RegionLifeClock::NoteCapWouldOverflow(RegionLifeClock::Carrier::RECEIPT);
        return Receipt::Status::LIFE_REGISTRY_FULL;
    }
    _to_lives[count].start = start;
    _to_lives[count].legacySeq = seq;
    _to_lives[count].lifeId = life;
    RegionLifeClock::Publish(RegionLifeClock::Carrier::RECEIPT, life);
    _to_life_n.store(static_cast<uint8_t>(count + 1), std::memory_order_release);
    return Receipt::Status::INSTALLED;
}

ZForwarding::Receipt ZForwarding::install_receipt_with_life(
    MAddress from, MAddress to, const std::function<void()>& beforeRegister)
{
    const MAddress existingBeforeLock = find(from);
    if (existingBeforeLock != 0) {
        return Receipt{ existingBeforeLock, false, Receipt::Status::EXISTING };
    }

    // The snapshot is intentionally taken before the deterministic test hook.
    // Correct code reloads it under _receiptInstallLock; the negative control
    // restores the old plain-count use and makes both rendezvoused installers
    // select the same slot.
    const uint8_t optimisticCount = _to_life_n.load(std::memory_order_relaxed);
    if (beforeRegister) {
        beforeRegister();
    }

    std::lock_guard<std::mutex> lock(_receiptInstallLock);
    const MAddress existing = find(from);
    if (existing != 0) {
        return Receipt{ existing, false, Receipt::Status::EXISTING };
    }
    const Receipt::Status lifeStatus = register_to_life_locked(to, optimisticCount);
    if (lifeStatus != Receipt::Status::INSTALLED && lifeStatus != Receipt::Status::EXISTING) {
        return Receipt{ 0, false, lifeStatus };
    }
    return insert_receipt(from, to);
}

bool ZForwarding::DestUsable(MAddress to)
{
    if (to == 0 || !Heap::IsHeapAddress(to)) {
        return false;
    }
    BaseObject* obj = reinterpret_cast<BaseObject*>(to);
    if (!obj->IsValidObject()) {
        return false;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr || toRegion->IsFreeRegion() || toRegion->IsGarbageRegion()) {
        return false;
    }
    if (to < toRegion->GetRegionStart() || to >= toRegion->GetRegionAllocPtr()) {
        return false;
    }
    const ObjectState::ObjectStateCode st = obj->GetObjectState().GetStateCode();
    return st != ObjectState::FORWARDED && st != ObjectState::FORWARDING;
}

MAddress ZForwarding::resolve_life(MAddress to) const
{
    if (to == 0) {
        return 0;
    }
    // Raw insert() is retained for pre-existing exempt/retired carriers whose
    // synthetic or non-heap destination has no RegionLifeId to validate.  The
    // product publication path cannot create such a receipt: InstallMapping
    // rejects DESTINATION_UNTRACKED before its release CAS.
    if (!Heap::IsHeapAddress(to)) {
        return to;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr || toRegion->IsFreeRegion() || toRegion->IsGarbageRegion()) {
        return 0;
    }
    const uint8_t toLifeCount = _to_life_n.load(std::memory_order_acquire);
    if (toLifeCount != 0) {
        const MAddress start = toRegion->GetRegionStart();
        const uint8_t seq = toRegion->GetRegionLifeSeq();
        const RegionLifeId life = toRegion->GetRegionLifeId();
        bool tracked = false;
        for (uint8_t i = 0; i < toLifeCount; ++i) {
            if (_to_lives[i].start == start) {
                tracked = true;
                const bool lifeCurrent = RegionLifeClock::Validate(
                    RegionLifeClock::Carrier::RECEIPT, _to_lives[i].lifeId, life);
                if (_to_lives[i].lifeId != life) {
                    StaleToLifeCount().fetch_add(1, std::memory_order_relaxed);
                }
                if (!lifeCurrent || _to_lives[i].legacySeq != seq) {
                    return 0;
                }
            }
        }
        if (!tracked) {
            RegionLifeClock::NoteUntracked(RegionLifeClock::Carrier::RECEIPT);
            if (RegionLifeClock::EnforceEnabled()) {
                return 0;
            }
        }
    } else {
        RegionLifeClock::NoteUntracked(RegionLifeClock::Carrier::RECEIPT);
        if (!RegionLifeClock::Validate(RegionLifeClock::Carrier::RECEIPT, 0,
                                       toRegion->GetRegionLifeId())) {
            return 0;
        }
    }
    return to;
}

MAddress ZForwarding::resolve_live(MAddress to) const
{
    to = resolve_life(to);
    if (to == 0) {
        return 0;
    }
    BaseObject* obj = reinterpret_cast<BaseObject*>(to);
    if (!obj->IsValidObject()) {
        return 0;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr || to < toRegion->GetRegionStart() || to >= toRegion->GetRegionAllocPtr()) {
        return 0;
    }
    if (DestUsable(to)) {
        return to;
    }
    ZForwarding* next = ForwardingTable::GetEntries(to);
    if (next == nullptr || next == this) {
        return 0;
    }
    const MAddress chained = next->find(to);
    if (chained == 0 || chained == to) {
        return 0;
    }
    return next->resolve_live(chained);
}

bool ZForwarding::receipt_live(MAddress to) const { return resolve_live(to) != 0; }

struct LookupCarrierWitness {
    MAddress start{ 0 };
    uint64_t publicationGeneration{ 0 };
    uint64_t fromPageEpoch{ 0 };
    RegionLifeId fromPageLifeId{ 0 };
    bool valid{ false };
};

static void CaptureLookupCarrier(ZForwarding* table, LookupCarrierWitness* witness)
{
    if (table == nullptr || witness == nullptr || witness->valid) {
        return;
    }
    witness->start = table->start();
    witness->publicationGeneration = table->publication_generation();
    const ZForwarding::FromPageView* fromPage = table->from_page_snapshot();
    if (fromPage != nullptr) {
        witness->fromPageEpoch = fromPage->epoch;
        witness->fromPageLifeId = fromPage->lifeId;
    }
    witness->valid = true;
}

static MAddress FindRetiredToImpl(MAddress from, ForwardingTable::ToAnswer* answer, bool require = false,
                                  uintptr_t* tableId = nullptr, LookupCarrierWitness* witness = nullptr)
{
    std::lock_guard<std::mutex> lock(g_retiredLock);
    bool searched = false;
    auto scan = [&](const std::vector<ZForwarding*>& gens) -> MAddress {
        for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
            ZForwarding* tab = *it;
            // A retired forwarding is independent of its source page. The page
            // may already have returned to the allocator, while this immutable
            // span and its entries remain live until relocation-set reset
            // (zRelocate.cpp:1041-1047; zRelocationSet.cpp:191-197).
            if (tab == nullptr || !tab->covers(from)) {
                continue;
            }
            searched = true;
            if (tableId != nullptr && *tableId == 0) {
                *tableId = reinterpret_cast<uintptr_t>(tab);
            }
            CaptureLookupCarrier(tab, witness);
            const MAddress to = tab->resolve_life(tab->find(from));
            if (to != 0) {
                if (require) {
                    tab->note_retired_required();
                }
                return to;
            }
        }
        return 0;
    };
    const MAddress fresh = scan(g_retired);
    if (fresh != 0) {
        if (answer != nullptr) {
            *answer = ForwardingTable::ToAnswer::ArmedHit;
        }
        return fresh;
    }
    const MAddress prev = scan(g_retiredPrev);
    if (prev != 0) {
        if (answer != nullptr) {
            *answer = ForwardingTable::ToAnswer::ArmedHit;
        }
        return prev;
    }
    if (answer != nullptr) {
        if (searched) {
            *answer = ForwardingTable::ToAnswer::ArmedMiss;
        } else {
            *answer = ForwardingTable::ToAnswer::Unarmed;
        }
    }
    return 0;
}

MAddress ForwardingTable::FindRetiredTo(MAddress from) { return FindRetiredToImpl(from, nullptr); }

MAddress ForwardingTable::RequireRetiredTo(MAddress from)
{
    return FindRetiredToImpl(from, nullptr, true);
}

MAddress ForwardingTable::FindTo(MAddress from)
{
    ZForwarding* tab = GetEntries(from);
    if (tab != nullptr) {
        if (!tab->covers(from)) {
            static std::atomic<uint64_t> g_findToUncovered{ 0 };
            const uint64_t n = g_findToUncovered.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8) {
                LOG(RTLOG_ERROR, "[FWDTABLE] FindTo !covers from=%p tabStart=%p size=%zu n=%llu",
                    reinterpret_cast<void*>(from), reinterpret_cast<void*>(tab->start()), tab->size(),
                    static_cast<unsigned long long>(n));
            }
        }
        const MAddress to = tab->resolve_life(tab->find(from));
        if (to != 0) {
            return to;
        }
    }
    return 0;
}

bool ForwardingTable::EntriesArmed(MAddress from) { return GetEntries(from) != nullptr; }

ForwardingTable::LookupResult ForwardingTable::LookupTo(MAddress from)
{
    // zForwarding.cpp:86-108,134-181. Resolve the active slot and retain it
    // under the same install lock that seals/unlinks it. The lookup may then
    // run lock-free while ClearEntries drains this exact ownership token.
    ZForwarding* retained = nullptr;
    bool activeCandidate = false;
    bool activeRejected = false;
    bool currentMembership = false;
    uintptr_t tableId = 0;
    LookupCarrierWitness carrierWitness;
    {
        std::lock_guard<std::mutex> lock(g_installLock);
        ZForwarding* candidate = Ready() ? MapGet(g_entries, from) : nullptr;
        currentMembership = Ready() && MapGet(g_membership, from) != nullptr;
        activeCandidate = candidate != nullptr;
        if (candidate != nullptr) {
            tableId = reinterpret_cast<uintptr_t>(candidate);
            CaptureLookupCarrier(candidate, &carrierWitness);
            if (!candidate->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY) ||
                !candidate->retain_page()) {
                activeRejected = true;
            } else {
                retained = candidate;
            }
        }
    }
    bool activeSearched = false;
    if (retained != nullptr) {
        activeSearched = true;
#if defined(MRT_TESTABLE_INTERNALS)
        ForwardingTable::LookupRetainHook hook = g_lookupRetainHook.load(std::memory_order_acquire);
        if (hook != nullptr) {
            hook(g_lookupRetainHookContext.load(std::memory_order_acquire));
        }
#endif
        const MAddress to = retained->resolve_life(retained->find(from));
        retained->release_page();
        if (to != 0) {
            g_armedHit.fetch_add(1, std::memory_order_relaxed);
            return { to, ToAnswer::ArmedHit, ToUnavailableCause::None, activeCandidate,
                     true, ToAnswer::ArmedHit, ToAnswer::Unarmed, false,
                     currentMembership, tableId, carrierWitness.start, carrierWitness.publicationGeneration,
                     carrierWitness.fromPageEpoch, carrierWitness.fromPageLifeId, carrierWitness.valid };
        }
    }
    ToAnswer retiredAnswer = ToAnswer::Unarmed;
    const MAddress retired = FindRetiredToImpl(from, &retiredAnswer, false, &tableId, &carrierWitness);
    if (retired != 0) {
        g_armedHit.fetch_add(1, std::memory_order_relaxed);
        return { retired, ToAnswer::ArmedHit, ToUnavailableCause::None, activeCandidate,
                 activeSearched, activeSearched ? ToAnswer::ArmedMiss : ToAnswer::Unarmed,
                 ToAnswer::ArmedHit, false, currentMembership, tableId, carrierWitness.start,
                 carrierWitness.publicationGeneration, carrierWitness.fromPageEpoch,
                 carrierWitness.fromPageLifeId, carrierWitness.valid };
    }
    // A carrier found in the active slot but refused by retain is a lifecycle
    // failure. It must not be downgraded to an ordinary armed miss merely
    // because the retired scan also saw ArmedMiss (zForwarding.cpp:171-181).
    const bool publicationClosed = PublicationClosedAt(from);
    if (activeRejected || retiredAnswer == ToAnswer::Unavailable || publicationClosed) {
        uint8_t causeBits = 0;
        if (activeRejected) {
            causeBits |= static_cast<uint8_t>(ToUnavailableCause::ActiveRetainRejected);
        }
        if (retiredAnswer == ToAnswer::Unavailable) {
            causeBits |= static_cast<uint8_t>(ToUnavailableCause::RetiredUnavailable);
        }
        if (publicationClosed) {
            causeBits |= static_cast<uint8_t>(ToUnavailableCause::PublicationClosed);
            if (retiredAnswer == ToAnswer::Unarmed) {
                causeBits |= static_cast<uint8_t>(ToUnavailableCause::TableDestroyed);
            } else if (retiredAnswer == ToAnswer::ArmedMiss) {
                causeBits |= static_cast<uint8_t>(ToUnavailableCause::NeverInstalled);
            }
        }
        const LookupResult result{
            0,
            ToAnswer::Unavailable,
            static_cast<ToUnavailableCause>(causeBits),
            activeCandidate,
            activeSearched,
            activeRejected ? ToAnswer::Unavailable
                           : (activeSearched ? ToAnswer::ArmedMiss : ToAnswer::Unarmed),
            retiredAnswer,
            publicationClosed,
            currentMembership,
            tableId,
            carrierWitness.start,
            carrierWitness.publicationGeneration,
            carrierWitness.fromPageEpoch,
            carrierWitness.fromPageLifeId,
            carrierWitness.valid,
        };
        g_unavailable.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (activeSearched || retiredAnswer == ToAnswer::ArmedMiss) {
        g_armedMiss.fetch_add(1, std::memory_order_relaxed);
        return { 0, ToAnswer::ArmedMiss, ToUnavailableCause::None, activeCandidate,
                 activeSearched, activeSearched ? ToAnswer::ArmedMiss : ToAnswer::Unarmed,
                 retiredAnswer, publicationClosed, currentMembership, tableId, carrierWitness.start,
                 carrierWitness.publicationGeneration, carrierWitness.fromPageEpoch,
                 carrierWitness.fromPageLifeId, carrierWitness.valid };
    }
    g_unarmed.fetch_add(1, std::memory_order_relaxed);
    return { 0, ToAnswer::Unarmed, ToUnavailableCause::None, activeCandidate, false,
             ToAnswer::Unarmed, retiredAnswer, publicationClosed, currentMembership, tableId, carrierWitness.start,
             carrierWitness.publicationGeneration, carrierWitness.fromPageEpoch,
             carrierWitness.fromPageLifeId, carrierWitness.valid };
}

ForwardingTable::NeverInstalledSnapshot ForwardingTable::CaptureNeverInstalledSnapshot(MAddress target)
{
    NeverInstalledSnapshot snapshot;

    // Keep the established install -> retired lock order.  The snapshot copies
    // scalar identity while every candidate remains protected from teardown.
    std::lock_guard<std::mutex> installLock(g_installLock);
    std::lock_guard<std::mutex> retiredLock(g_retiredLock);

    auto visit = [&](ZForwarding* tab, bool active) {
        if (tab == nullptr) {
            return;
        }
        CarrierState state;
        if (!active) {
            state = CarrierState::Retired;
        } else if (tab->is_provisional()) {
            state = CarrierState::ActiveUnpublished;
        } else if (PublicationOpen(PublicationStateAt(tab->start()))) {
            state = CarrierState::ActiveOpen;
        } else {
            state = CarrierState::ActiveClosed;
        }

        if (tab->covers(target)) {
            const MAddress to = tab->resolve_life(tab->find(target));
            ++snapshot.carrierTotal;
            if (snapshot.carrierCount < kNeverInstalledCarrierLimit) {
                CarrierIdentity& out = snapshot.carriers[snapshot.carrierCount++];
                out.tableId = reinterpret_cast<uintptr_t>(tab);
                out.start = tab->start();
                out.size = tab->size();
                out.tableGeneration = tab->table_generation();
                out.publicationGeneration = tab->publication_generation();
                const ZForwarding::FromPageView* fromPage = tab->from_page_snapshot();
                if (fromPage != nullptr) {
                    out.fromPageEpoch = fromPage->epoch;
                    out.fromPageLifeId = fromPage->lifeId;
                }
                out.state = state;
                out.answer = to == 0 ? ToAnswer::ArmedMiss : ToAnswer::ArmedHit;
                out.pendingDestroy = !active && RetiredDestroyEligible(tab);
            } else {
                snapshot.carrierOverflow = true;
            }
        }

        // Raw header cannot distinguish an ordinary Usable object from an
        // already-remapped to-object.  Reverse scan is therefore performed
        // only here, on the diagnostic fail-closed path; publication remains
        // unchanged and no reverse index is maintained on the hot path.
        MAddress receiptFrom = 0;
        if (tab->find_from_by_to(target, &receiptFrom)) {
            ++snapshot.reverseTotal;
            if (snapshot.reverseCount < kNeverInstalledReverseLimit) {
                ReverseReceiptIdentity& out = snapshot.reverseReceipts[snapshot.reverseCount++];
                out.tableId = reinterpret_cast<uintptr_t>(tab);
                out.publicationGeneration = tab->publication_generation();
                out.from = receiptFrom;
            } else {
                snapshot.reverseOverflow = true;
            }
        }
    };
    auto visitActiveMap = [&](const ZGranuleMap<ZForwarding*>& map) {
        ZForwarding* previous = nullptr;
        for (size_t i = 0; i < map.size(); ++i) {
            ZForwarding* tab = map.get(static_cast<zoffset>(i * map.granule()));
            if (tab != previous) {
                visit(tab, true);
                previous = tab;
            }
        }
    };
    // These are exactly LookupTo's queryable carrier domains. Membership is a
    // second pointer to an active or retired carrier, not another carrier; do
    // not enumerate it and then need a bounded dedup ledger which could hide a
    // later covering table.
    visitActiveMap(g_entries);
    for (auto it = g_retired.rbegin(); it != g_retired.rend(); ++it) {
        visit(*it, false);
    }
    for (auto it = g_retiredPrev.rbegin(); it != g_retiredPrev.rend(); ++it) {
        visit(*it, false);
    }
    return snapshot;
}

uint64_t ForwardingTable::ArmedHitCount() { return g_armedHit.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::ArmedMissCount() { return g_armedMiss.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::UnavailableCount() { return g_unavailable.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::UnarmedCount() { return g_unarmed.load(std::memory_order_relaxed); }

#if defined(MRT_TESTABLE_INTERNALS)
void ForwardingTable::SetLookupRetainHook(LookupRetainHook hook, void* context)
{
    g_lookupRetainHookContext.store(context, std::memory_order_release);
    g_lookupRetainHook.store(hook, std::memory_order_release);
}

void ForwardingTable::ForcePublicationClosedForTest(MAddress address)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* active = Ready() ? MapGet(g_entries, address) : nullptr;
    CHECK_DETAIL(active != nullptr && active->covers(address),
                 "NeverInstalled test fault needs an active covering carrier address=%p",
                 reinterpret_cast<void*>(address));
    SealPublicationLocked(active->start(), active->size());
}

void ForwardingTable::SetReceiptLifeRegisterHook(ReceiptLifeRegisterHook hook, void* context)
{
    g_receiptLifeRegisterHookContext.store(context, std::memory_order_release);
    g_receiptLifeRegisterHook.store(hook, std::memory_order_release);
}
#endif

void ForwardingTable::NoteCompare(MAddress addr, bool legacy)
{
    if (!Ready()) {
        return;
    }
    const bool table = get(addr) != nullptr;
    const uint64_t n = g_cmpTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (table == legacy) {
        g_cmpAgree.fetch_add(1, std::memory_order_relaxed);
    } else {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
        const unsigned rtype = region == nullptr ? kTypeBuckets - 1
                                                 : static_cast<unsigned>(region->GetRegionType());
        const unsigned bucket = rtype < kTypeBuckets ? rtype : kTypeBuckets - 1;
        const unsigned ghost = (region != nullptr && region->IsGhostFromRegion()) ? 1u : 0u;
        if (table) {
            const uint64_t c = g_cmpTableOnly.fetch_add(1, std::memory_order_relaxed) + 1;
            g_tableOnlyByType[bucket].fetch_add(1, std::memory_order_relaxed);
            if (c <= 64) {
                LOG(RTLOG_ERROR, "[FWDTABLE][tableOnly] n=%lu addr=%#zx rtype=%u ghost=%u", c,
                    static_cast<size_t>(addr), rtype, ghost);
            }
        } else {
            const uint64_t c = g_cmpLegacyOnly.fetch_add(1, std::memory_order_relaxed) + 1;
            g_legacyOnlyByType[bucket].fetch_add(1, std::memory_order_relaxed);
            if (c <= 64) {
                LOG(RTLOG_ERROR, "[FWDTABLE][legacyOnly] n=%lu addr=%#zx rtype=%u ghost=%u", c,
                    static_cast<size_t>(addr), rtype, ghost);
            }
        }
    }
    if ((n & (n - 1)) == 0) {
        DumpCompare("periodic");
    }
}

void ForwardingTable::NoteDestCompare(MAddress from, MAddress geometricTo)
{
    if (!Ready()) {
        return;
    }
    const MAddress stored = FindTo(from);
    const uint64_t n = g_destTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stored == 0) {
        g_destPending.fetch_add(1, std::memory_order_relaxed);
    } else if (stored == geometricTo) {
        g_destAgree.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_destDisagree.fetch_add(1, std::memory_order_relaxed);
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(from);
        const unsigned rtype = region == nullptr ? kTypeBuckets - 1
                                                 : static_cast<unsigned>(region->GetRegionType());
        const unsigned bucket = rtype < kTypeBuckets ? rtype : kTypeBuckets - 1;
        g_destDisagreeByType[bucket].fetch_add(1, std::memory_order_relaxed);
        LOG(RTLOG_ERROR, "[FWDENT][disagree] from=%#zx table=%#zx geo=%#zx rtype=%u", static_cast<size_t>(from),
            static_cast<size_t>(stored), static_cast<size_t>(geometricTo), rtype);
    }
    if ((n & (n - 1)) == 0) {
        DumpCompare("dest-periodic");
    }
}

void ForwardingTable::DumpCompare(const char* why)
{
    if (!Ready()) {
        return;
    }
    LOG(RTLOG_ERROR, "[FWDTABLE][cmp] why=%s total=%lu agree=%lu tableOnly=%lu legacyOnly=%lu",
        why == nullptr ? "?" : why, g_cmpTotal.load(std::memory_order_relaxed),
        g_cmpAgree.load(std::memory_order_relaxed), g_cmpTableOnly.load(std::memory_order_relaxed),
        g_cmpLegacyOnly.load(std::memory_order_relaxed));
    LOG(RTLOG_ERROR, "[FWDENT][dest] why=%s total=%lu agree=%lu disagree=%lu pending=%lu",
        why == nullptr ? "?" : why, g_destTotal.load(std::memory_order_relaxed),
        g_destAgree.load(std::memory_order_relaxed), g_destDisagree.load(std::memory_order_relaxed),
        g_destPending.load(std::memory_order_relaxed));
    LOG(RTLOG_ERROR, "[FWDENT][sole] why=%s armedHit=%lu armedMiss=%lu unarmed=%lu", why == nullptr ? "?" : why,
        g_armedHit.load(std::memory_order_relaxed), g_armedMiss.load(std::memory_order_relaxed),
        g_unarmed.load(std::memory_order_relaxed));
    for (unsigned t = 0; t < kTypeBuckets; ++t) {
        const uint64_t to = g_tableOnlyByType[t].load(std::memory_order_relaxed);
        const uint64_t lo = g_legacyOnlyByType[t].load(std::memory_order_relaxed);
        const uint64_t dd = g_destDisagreeByType[t].load(std::memory_order_relaxed);
        if (to != 0 || lo != 0 || dd != 0) {
            LOG(RTLOG_ERROR, "[FWDTABLE][bytype] rtype=%u tableOnly=%lu legacyOnly=%lu destDisagree=%lu", t, to, lo,
                dd);
        }
    }
}
} // namespace MapleRuntime
