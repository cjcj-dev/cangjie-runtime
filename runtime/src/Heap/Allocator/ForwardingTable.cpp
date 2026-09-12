// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/ForwardingTable.h"

#include <algorithm>
#include <atomic>
#include <functional>
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
#include "Heap/Collector/Collector.h"
#include "Heap/Allocator/ZGranuleMap.h"
#include "Heap/Verify/M0Correlation.h"
#include "Heap/WCollector/WCollector.h"

namespace MapleRuntime {
namespace {

// zGeneration.cpp:276-284 / zRelocationSet.cpp:172-200.
// Each generation owns its installed forwarding objects and their common arena.
// The map borrows them until that generation resets its relocation set.
ZGranuleMap<ZForwarding*> g_entries;
struct RelocationSet {
    std::unique_ptr<ForwardingAllocator> arena;
    std::vector<ZForwarding*> forwardings;
};
RelocationSet g_relocationSets[2];

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

#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<ForwardingTable::LookupRetainHook> g_lookupRetainHook{ nullptr };
std::atomic<void*> g_lookupRetainHookContext{ nullptr };
#endif

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
            g_entries.ResetForTest();
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
    if (!g_entries.Initialize(heapStart, heapSize, unitSize)) {
        LOG(RTLOG_ERROR, "[FWDTABLE] granule map init failed size=%zu unit=%zu -- table stays off", heapSize,
            unitSize);
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    LOG(RTLOG_ERROR, "[FWDTABLE] armed base=%#zx size=%zu unit=%zu entries=%zu", static_cast<size_t>(heapStart),
        heapSize, unitSize, g_entries.size());
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

bool ForwardingTable::BeginForwardingArena(Generation gen, RegionList& regions)
{
    // zRelocationSet.cpp:79-134: allocate the entire selected set before copy.
    RelocationSet& set = g_relocationSets[static_cast<size_t>(gen)];
    CHECK(set.forwardings.empty());
    size_t budget = 0;
    bool valid = true;
    regions.VisitAllRegions([&](RegionInfo* region) {
        const size_t entries = ZForwarding::nentries(ObjectCountUpperBound(region, region->GetRegionSize()));
        size_t bytes;
        valid = valid && entries != 0 && ZForwarding::AttachedArray::allocation_size(entries, &bytes) &&
            ForwardingAllocator::add_to_budget(bytes, &budget);
    });
    if (!valid) return false;
    set.arena = std::make_unique<ForwardingAllocator>(budget);
    if (!set.arena->valid()) return false;
    regions.VisitAllRegions([&](RegionInfo* region) {
        ZForwarding* forwarding = ZForwarding::alloc(
            ObjectCountUpperBound(region, region->GetRegionSize()), region->GetRegionStart(),
            g_entries.base(), region->GetRegionSize(), region, region->GetRegionLifeId(), false, set.arena.get());
        CHECK(forwarding != nullptr);
        forwarding->set_table_generation(static_cast<uint8_t>(gen));
        set.forwardings.push_back(forwarding);
        insert(forwarding);
    });
    return true;
}

size_t ForwardingTable::ObjectCountUpperBound(RegionInfo* region, size_t regionSize)
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
    return static_cast<size_t>(estimate);
}

void ForwardingTable::insert(ZForwarding* forwarding)
{
    // zForwardingTable.inline.hpp:48-54
    if (forwarding == nullptr || !Ready()) {
        return;
    }
    MapPut(g_entries, forwarding->start(), forwarding->size(), forwarding);
}

void ForwardingTable::remove(ZForwarding* forwarding)
{
    if (forwarding != nullptr && Ready() && MapGet(g_entries, forwarding->start()) == forwarding) {
        MapPut(g_entries, forwarding->start(), forwarding->size(), nullptr);
    }
}


ZForwarding* ForwardingTable::get(MAddress addr)
{
    return Ready() ? MapGet(g_entries, addr) : nullptr;
}


bool ForwardingTable::InstallPublicationBeforeCopy(
    MAddress regionStart, size_t regionSize, RegionInfo* region)
{
    ZForwarding* forwarding = get(regionStart);
    return forwarding != nullptr && forwarding->start() == regionStart &&
        forwarding->size() == regionSize && forwarding->page() == region && forwarding->page_life_current();
}

void ForwardingTable::ResetRelocationSet(Generation gen)
{
    // The generation reaches this edge after its last mark/remap consumer.
    // Page retains protect source bytes; they do not extend forwarding lifetime.
    std::lock_guard<std::mutex> lock(g_installLock);
    RelocationSet& set = g_relocationSets[static_cast<size_t>(gen)];
    for (ZForwarding* forwarding : set.forwardings) {
        remove(forwarding);
    }
    for (ZForwarding* forwarding : set.forwardings) {
        RegionInfo* page = RegionInfo::TryGetRegionInfoAt(forwarding->start());
        if (page != nullptr && page->metadata.fwdOwner.load(std::memory_order_acquire) == forwarding) {
            page->metadata.fwdOwner.store(nullptr, std::memory_order_release);
        }
        forwarding->~ZForwarding();
    }
    set.forwardings.clear();
    set.arena.reset();
}

ForwardingTable::Publication ForwardingTable::RetainCovering(MAddress from)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* tab = GetCovering(from);
    if (tab == nullptr) {
        return Publication();
    }
    return Publication(tab);
}

ZForwarding* ForwardingTable::GetEntries(MAddress addr) { return get(addr); }

ZForwarding* ForwardingTable::GetCovering(MAddress addr) { return get(addr); }

void ForwardingTable::VisitAll(const std::function<void(ZForwarding*)>& visitor)
{
    if (Ready() && visitor != nullptr) g_entries.visit_unique(visitor);
}

bool ForwardingTable::PublishFromPageView(RegionInfo* region, LiveInfo* liveInfo, uint64_t epoch,
                                          MAddress topAtStart, MAddress markStartAllocPtr,
                                          uint64_t liveByteCount, uint8_t owner,
                                          uint8_t largeMarked, RegionLifeId lifeId)
{
    if (region == nullptr || lifeId == 0 || region->GetRegionLifeId() != lifeId) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* carrier = GetEntries(region->GetRegionStart());
    if (carrier == nullptr || carrier->page() != region) {
        return false;
    }
    carrier->publish_from_page_view(liveInfo, epoch, topAtStart, markStartAllocPtr,
                                    liveByteCount, owner, largeMarked, lifeId);
    ZForwarding* previous = region->metadata.fwdOwner.load(std::memory_order_acquire);
    if (previous != carrier) {
        CHECK_DETAIL(UnbindPageOwnerLocked(region, false),
                     "replacing retained forwarding owner region=%p", region);
        region->metadata.fwdOwner.store(carrier, std::memory_order_release);
    }
    return true;
}

ForwardingTable::Owner ForwardingTable::RetainPageOwner(const RegionInfo* region)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    return Owner(region == nullptr ? nullptr : region->metadata.fwdOwner.load(std::memory_order_acquire));
}

bool ForwardingTable::UnbindPageOwnerLocked(RegionInfo* region, bool allowExclusive)
{
    ZForwarding* owner = region->metadata.fwdOwner.load(std::memory_order_acquire);
    if (owner == nullptr) return true;
    int32_t refs = owner->ref_count().load(std::memory_order_acquire);
    // A prepared but never submitted page has only its construction token.
    // No queue/scope may carry it and no payload reader may remain. Finish
    // that token rather than resetting a live forwarding to an idle state.
    if (refs == 1 && owner->claim()) {
        owner->release_page();
        owner->mark_done();
        refs = 0;
    }
    if (refs != 0 && !(allowExclusive && refs == -1)) return false;
    region->metadata.fwdOwner.store(nullptr, std::memory_order_release);
    return true;
}

void ForwardingTable::ClearPageOwner(RegionInfo* region)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    CHECK_DETAIL(UnbindPageOwnerLocked(region, true), "clearing retained forwarding owner region=%p", region);
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

bool ZForwarding::page_life_current() const
{
    if (_page == nullptr) {
        return true;
    }
    return _page_life_id == _page->GetRegionLifeId();
}

ForwardingTable::Publication ForwardingTable::EnsurePublicationBeforeCopy(
    RegionInfo* region, MAddress from)
{
    return RetainOpenPublicationAfterCopy(region, from);
}

ForwardingTable::Publication ForwardingTable::RetainOpenPublicationAfterCopy(
    RegionInfo* region, MAddress from)
{
    ZForwarding* forwarding = get(from);
    return forwarding != nullptr && forwarding->page() == region && forwarding->page_life_current()
        ? Publication(forwarding) : Publication();
}

ZForwarding::Receipt ForwardingTable::InstallMapping(
    const Publication& publication, MAddress from, MAddress to)
{
    ZForwarding* tab = publication.forwarding;
    CHECK_DETAIL(tab != nullptr && !tab->is_provisional() && tab->covers(from),
                 "forwarding publication responsibility missing from=%#zx to=%#zx tab=%p",
                 static_cast<size_t>(from), static_cast<size_t>(to), tab);
    const ZForwarding::Receipt receipt = tab->insert_receipt(from, to);
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

MAddress ZForwarding::resolve_live(MAddress to) const
{
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

MAddress ForwardingTable::FindTo(MAddress from)
{
    std::lock_guard<std::mutex> lock(g_installLock);
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
        const MAddress to = tab->find(from);
        if (to != 0) {
            return to;
        }
    }
    return 0;
}

bool ForwardingTable::EntriesArmed(MAddress from) { return GetEntries(from) != nullptr; }

ForwardingTable::LookupResult ForwardingTable::LookupTo(MAddress from)
{
    ZForwarding* forwarding = get(from);
    LookupCarrierWitness witness;
    CaptureLookupCarrier(forwarding, &witness);
    const MAddress to = forwarding == nullptr ? 0 : forwarding->find(from);
    const ToAnswer answer = forwarding == nullptr ? ToAnswer::Unarmed
        : (to == 0 ? ToAnswer::ArmedMiss : ToAnswer::ArmedHit);
    if (answer == ToAnswer::ArmedHit) g_armedHit.fetch_add(1, std::memory_order_relaxed);
    else if (answer == ToAnswer::ArmedMiss) g_armedMiss.fetch_add(1, std::memory_order_relaxed);
    else g_unarmed.fetch_add(1, std::memory_order_relaxed);
    return {to, answer, ToUnavailableCause::None, forwarding != nullptr, forwarding != nullptr,
            answer, ToAnswer::Unarmed, false, forwarding != nullptr,
            reinterpret_cast<uintptr_t>(forwarding), witness.start, witness.publicationGeneration,
            witness.fromPageEpoch, witness.fromPageLifeId, witness.valid};
}

ForwardingTable::NeverInstalledSnapshot ForwardingTable::CaptureNeverInstalledSnapshot(MAddress target)
{
    NeverInstalledSnapshot snapshot;

    // Keep the established install -> retired lock order.  The snapshot copies
    // scalar identity while every candidate remains protected from teardown.
    std::lock_guard<std::mutex> installLock(g_installLock);

    auto visit = [&](ZForwarding* tab, bool active) {
        if (tab == nullptr) {
            return;
        }
        CarrierState state;
        if (!active) {
            state = CarrierState::Retired;
        } else if (tab->is_provisional()) {
            state = CarrierState::ActiveUnpublished;
        } else {
            state = CarrierState::ActiveOpen;
        }

        if (tab->covers(target)) {
            const MAddress to = tab->find(target);
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
