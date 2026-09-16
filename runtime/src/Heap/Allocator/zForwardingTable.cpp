// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zForwardingTable.hpp"

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
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGranuleMap.hpp"
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
    return true;
}

bool ForwardingTable::BeginForwardingArena(Generation gen, RegionList& regions)
{
    // zRelocationSet.cpp:79-134: allocate the entire selected set before copy.
    RelocationSet& set = g_relocationSets[static_cast<size_t>(gen)];
    CHECK(set.forwardings.empty());
    size_t budget = 0;
    bool valid = true;
    regions.VisitAllRegions([&](ZPage* region) {
        CHECK(region->GetOwnerGeneration() == gen);
        const size_t entries = ZForwarding::nentries(ObjectCountUpperBound(region, region->GetRegionSize()));
        size_t bytes;
        valid = valid && entries != 0 && ZForwarding::AttachedArray::allocation_size(entries, &bytes) &&
            ForwardingAllocator::add_to_budget(bytes, &budget);
    });
    if (!valid) return false;
    set.arena = std::make_unique<ForwardingAllocator>(budget);
    if (!set.arena->valid()) return false;
    regions.VisitAllRegions([&](ZPage* region) {
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

size_t ForwardingTable::ObjectCountUpperBound(ZPage* region, size_t regionSize)
{
    // ZForwarding::nentries sizes the attached array from live object count.
    // Before a mark count is authoritative (standalone setup), retain the
    // existing capacity upper bound; selected product pages use their livemap.
    if (!region->is_marked()) {
        return regionSize >> ZForwarding::kAlignShift;
    }
    return region->live_objects();
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
    MAddress regionStart, size_t regionSize, ZPage* region, Generation gen)
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
        ZPage* page = Heap::page(forwarding->start());
        if (page != nullptr && page->_scratch.fwdOwner.load(std::memory_order_acquire) == forwarding) {
            page->_scratch.fwdOwner.store(nullptr, std::memory_order_release);
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
    auto& map = g_relocationSets[static_cast<size_t>(generation)].map;
    ZForwarding* last = nullptr;
    for (size_t i = 0; i < map.size(); ++i) {
        ZForwarding* value = map.at(i);
        if (value != nullptr && value != last) {
            visitor(value);
            last = value;
        }
    }
}

bool ForwardingTable::PublishFromPageView(ZPage* region, ZLiveMap* livemap, uint64_t epoch,
                                          MAddress topAtStart, uint64_t birthSequence,
                                          uint8_t owner,
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
    carrier->publish_from_page_view(livemap, epoch, topAtStart, birthSequence,
                                    owner, largeMarked, lifeId);
    ZForwarding* previous = region->_scratch.fwdOwner.load(std::memory_order_acquire);
    if (previous != carrier) {
        CHECK_DETAIL(UnbindPageOwnerLocked(region, false),
                     "replacing retained forwarding owner region=%p", region);
        region->_scratch.fwdOwner.store(carrier, std::memory_order_release);
    }
    return true;
}

ForwardingTable::Owner ForwardingTable::RetainPageOwner(const ZPage* region)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    return Owner(region == nullptr ? nullptr : region->_scratch.fwdOwner.load(std::memory_order_acquire));
}

bool ForwardingTable::UnbindPageOwnerLocked(ZPage* region, bool allowExclusive)
{
    ZForwarding* owner = region->_scratch.fwdOwner.load(std::memory_order_acquire);
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
    region->_scratch.fwdOwner.store(nullptr, std::memory_order_release);
    return true;
}

void ForwardingTable::ClearPageOwner(ZPage* region)
{
    std::lock_guard<std::mutex> lock(g_installLock);
    CHECK_DETAIL(UnbindPageOwnerLocked(region, true), "clearing retained forwarding owner region=%p", region);
}

const ZForwarding::FromPageView* ForwardingTable::GetFromPageView(ZPage* region)
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
    ZPage* region, MAddress from)
{
    return RetainOpenPublicationAfterCopy(region, from);
}

ForwardingTable::Publication ForwardingTable::RetainOpenPublicationAfterCopy(
    ZPage* region, MAddress from)
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

bool ZForwarding::DestUsable(MAddress to)
{
    if (to == 0 || !Heap::IsHeapAddress(to)) {
        return false;
    }
    BaseObject* obj = reinterpret_cast<BaseObject*>(to);
    if (!obj->IsValidObject()) {
        return false;
    }
    ZPage* toRegion = Heap::page(to);
    if (toRegion == nullptr || toRegion->IsFreeRegion() || toRegion->IsGarbageRegion()) {
        return false;
    }
    if (to < toRegion->GetRegionStart() || to >= toRegion->GetRegionAllocPtr()) {
        return false;
    }
    const ObjectState::ObjectStateCode st = obj->GetObjectState().GetStateCode();
    return st != ObjectState::FORWARDED && st != ObjectState::FORWARDING;
}

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
    // ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp:131)
    // routes through the source generation's forwarding, independently of
    // the current page installed by promotion. Keep the carrier while using
    // its source identity; a cleared owner supplies no routing authority.
    ZPage* region = Heap::page(from);
    auto carrier = RetainPageOwner(region);
    if (carrier) {
        const auto* source = carrier->from_page_view(region->GetRegionLifeId());
        if (source != nullptr) {
            gen = static_cast<Generation>(source->owner);
        }
    }
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

#include "Heap/z/zForwardingTable.inline.hpp"
