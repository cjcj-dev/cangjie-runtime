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
struct RelocationSet {
    ZGranuleMap<ZForwarding*> map;
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
// Serialize page facade publication with generation reset.
std::mutex g_installLock;
std::atomic<uint64_t> g_armedHit{ 0 };
std::atomic<uint64_t> g_armedMiss{ 0 };
std::atomic<uint64_t> g_unarmed{ 0 };



} // namespace

bool ForwardingTable::Ready() { return g_ready.load(std::memory_order_acquire); }

bool ForwardingTable::Initialize(MAddress heapStart, size_t heapSize, size_t unitSize)
{
    if (g_ready.load(std::memory_order_acquire)) {
#if defined(MRT_GC_UNIT_TESTS)
        if (g_relocationSets[0].map.base() != heapStart) {
            // Aggregate gc_unit creates a fresh synthetic mmap per test. Each
            // fixture destructor has already drained/reclaimed its carriers;
            // rebase only the test build so the next fixture exercises the
            // same product address checks rather than an obsolete map base.
            g_relocationSets[0].map.ResetForTest();
            g_relocationSets[1].map.ResetForTest();
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
    if (!g_relocationSets[0].map.Initialize(heapStart, heapSize, unitSize) ||
        !g_relocationSets[1].map.Initialize(heapStart, heapSize, unitSize)) {
        LOG(RTLOG_ERROR, "[FWDTABLE] granule map init failed size=%zu unit=%zu -- table stays off", heapSize,
            unitSize);
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    LOG(RTLOG_ERROR, "[FWDTABLE] armed base=%#zx size=%zu unit=%zu entries=%zu", static_cast<size_t>(heapStart),
        heapSize, unitSize, g_relocationSets[0].map.size());
    static std::atomic<bool> dumped{ false };
    bool expected = false;
    if (dumped.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[FWDTABLE][refuse] atexit full=%llu overflow=%llu fallbackFull=%llu "
                         "fallbackOverflow=%llu armedHit=%llu armedMiss=%llu unarmed=%llu\n",
                         static_cast<unsigned long long>(ZForwarding::FullRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::OverflowRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::FullFallbacks().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::OverflowFallbacks().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(ForwardingTable::ArmedHitCount()),
                         static_cast<unsigned long long>(ForwardingTable::ArmedMissCount()),
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
        CHECK(region->GetOwnerGeneration() == gen);
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
            set.map.base(), region->GetRegionSize(), region, region->GetRegionLifeId(), set.arena.get());
        CHECK(forwarding != nullptr);
        forwarding->set_table_generation(static_cast<uint8_t>(gen));
        set.forwardings.push_back(forwarding);
        insert(forwarding);
    });
    return true;
}

#if defined(MRT_TESTABLE_INTERNALS)
const ForwardingAllocator* ForwardingTable::ArenaForTest(Generation gen)
{
    return g_relocationSets[static_cast<size_t>(gen)].arena.get();
}
#endif

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
    auto& map = g_relocationSets[forwarding->table_generation()].map;
    CHECK(MapGet(map, forwarding->start()) == nullptr);
    MapPut(map, forwarding->start(), forwarding->size(), forwarding);
}

void ForwardingTable::remove(ZForwarding* forwarding)
{
    if (forwarding == nullptr || !Ready()) return;
    auto& map = g_relocationSets[forwarding->table_generation()].map;
    CHECK(MapGet(map, forwarding->start()) == forwarding);
    MapPut(map, forwarding->start(), forwarding->size(), nullptr);
}

ZForwarding* ForwardingTable::get(MAddress addr, Generation gen)
{
    return Ready() ? MapGet(g_relocationSets[static_cast<size_t>(gen)].map, addr) : nullptr;
}

bool ForwardingTable::InstallPublicationBeforeCopy(
    MAddress regionStart, size_t regionSize, RegionInfo* region, Generation gen)
{
    ZForwarding* forwarding = get(regionStart, gen);
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

ZForwarding* ForwardingTable::GetEntries(MAddress addr, Generation gen) { return get(addr, gen); }

ZForwarding* ForwardingTable::GetCovering(MAddress addr, Generation gen) { return get(addr, gen); }

void ForwardingTable::VisitAll(Generation generation, const std::function<void(ZForwarding*)>& visitor)
{
    if (!Ready() || visitor == nullptr) return;
    g_relocationSets[static_cast<size_t>(generation)].map.visit_unique(visitor);
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
    ZForwarding* carrier = get(region->GetRegionStart(), static_cast<Generation>(owner));
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
    if (region == nullptr) return nullptr;
    auto carrier = RetainPageOwner(region);
    return carrier ? carrier->from_page_view(region->GetRegionLifeId()) : nullptr;
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
    auto owner = RetainPageOwner(region);
    ZForwarding* forwarding = owner.get();
    return forwarding != nullptr && forwarding->covers(from) && forwarding->page_life_current()
        ? Publication(forwarding) : Publication();
}

ZForwarding::Receipt ForwardingTable::InstallMapping(
    const Publication& publication, MAddress from, MAddress to)
{
    ZForwarding* tab = publication.forwarding;
    CHECK_DETAIL(tab != nullptr && tab->covers(from),
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
    ZForwarding* next = ForwardingTable::RetainPageOwner(toRegion).get();
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
    const ZForwarding::FromPageView* fromPage = table->from_page_snapshot();
    if (fromPage != nullptr) {
        witness->fromPageEpoch = fromPage->epoch;
        witness->fromPageLifeId = fromPage->lifeId;
    }
    witness->valid = true;
}

MAddress ForwardingTable::FindTo(MAddress from, Generation gen)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    ZForwarding* tab = get(from, gen);
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

bool ForwardingTable::EntriesArmed(MAddress from, Generation gen) { return get(from, gen) != nullptr; }

ForwardingTable::LookupResult ForwardingTable::LookupTo(MAddress from, Generation gen)
{
    return LookupForwarding(from, get(from, gen));
}

ForwardingTable::LookupResult ForwardingTable::LookupForwarding(MAddress from, ZForwarding* forwarding)
{
    LookupCarrierWitness witness;
    CaptureLookupCarrier(forwarding, &witness);
    const MAddress to = forwarding == nullptr ? 0 : forwarding->find(from);
    const ToAnswer answer = forwarding == nullptr ? ToAnswer::Unarmed
        : (to == 0 ? ToAnswer::ArmedMiss : ToAnswer::ArmedHit);
    if (answer == ToAnswer::ArmedHit) g_armedHit.fetch_add(1, std::memory_order_relaxed);
    else if (answer == ToAnswer::ArmedMiss) g_armedMiss.fetch_add(1, std::memory_order_relaxed);
    else g_unarmed.fetch_add(1, std::memory_order_relaxed);
    return {to, answer, forwarding != nullptr,
            answer, forwarding != nullptr,
            reinterpret_cast<uintptr_t>(forwarding), witness.start,
            witness.fromPageEpoch, witness.fromPageLifeId, witness.valid};
}

ForwardingTable::NeverInstalledSnapshot ForwardingTable::CaptureNeverInstalledSnapshot(MAddress target)
{
    NeverInstalledSnapshot snapshot;

    // Capture identities before generation reset can destroy the set.
    std::lock_guard<std::mutex> installLock(g_installLock);

    auto visit = [&](ZForwarding* tab) {
        if (tab == nullptr) {
            return;
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
                const ZForwarding::FromPageView* fromPage = tab->from_page_snapshot();
                if (fromPage != nullptr) {
                    out.fromPageEpoch = fromPage->epoch;
                    out.fromPageLifeId = fromPage->lifeId;
                }
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
                visit(tab);
                previous = tab;
            }
        }
    };
    // The lookup domain is precisely the installed forwarding map.
    for (const auto& set : g_relocationSets) visitActiveMap(set.map);
    return snapshot;
}

uint64_t ForwardingTable::ArmedHitCount() { return g_armedHit.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::ArmedMissCount() { return g_armedMiss.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::UnarmedCount() { return g_unarmed.load(std::memory_order_relaxed); }



} // namespace MapleRuntime
