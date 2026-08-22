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
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/ZGranuleMap.h"
#include "Heap/Collector/RelocationSetTxn.h"

namespace MapleRuntime {
namespace {

// zForwardingTable.hpp:32-52 — one granule map of ZForwarding*.
// Two maps because Dispel unlinks membership while ClearEntries (a phase later)
// is when the attached array may be retired. ZGC has one map: reset_relocation_set
// is the only unlink (zGeneration.cpp:276-285).
ZGranuleMap<ZForwarding*> g_membership;
ZGranuleMap<ZForwarding*> g_entries;
std::atomic<bool> g_ready{ false };

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
std::atomic<uint64_t> g_unarmed{ 0 };

ZForwarding* ReaderEntries(MAddress addr, RelocationSetTxn::Handle& handle)
{
    if (RelocationSetTxn::Enabled()) {
        handle = RelocationSetTxn::AcquireForAddress(addr);
        ZForwarding* forwarding = handle ? handle.GetEnvelope() : nullptr;
        if (forwarding == nullptr || !forwarding->covers(addr) ||
            !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
            return nullptr;
        }
        return forwarding;
    }
    ZForwarding* forwarding = g_entries.get(addr);
    if (forwarding != nullptr && !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
        return nullptr;
    }
    return forwarding;
}

} // namespace

bool ForwardingTable::Ready() { return g_ready.load(std::memory_order_acquire); }

void ForwardingTable::Initialize(MAddress heapStart, size_t heapSize, size_t unitSize)
{
    if (g_ready.load(std::memory_order_acquire) || unitSize == 0 || heapSize == 0) {
        return;
    }
    if (!g_membership.Initialize(heapStart, heapSize, unitSize) ||
        !g_entries.Initialize(heapStart, heapSize, unitSize)) {
        LOG(RTLOG_ERROR, "[FWDTABLE] granule map init failed size=%zu unit=%zu -- table stays off", heapSize,
            unitSize);
        return;
    }
    g_ready.store(true, std::memory_order_release);
    LOG(RTLOG_ERROR, "[FWDTABLE] armed base=%#zx size=%zu unit=%zu entries=%zu", static_cast<size_t>(heapStart),
        heapSize, unitSize, g_membership.size());
    static std::atomic<bool> dumped{ false };
    bool expected = false;
    if (dumped.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[FWDTABLE][refuse] atexit full=%llu overflow=%llu armedHit=%llu armedMiss=%llu unarmed=%llu\n",
                         static_cast<unsigned long long>(ZForwarding::FullRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             ZForwarding::OverflowRefusals().load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(ForwardingTable::ArmedHitCount()),
                         static_cast<unsigned long long>(ForwardingTable::ArmedMissCount()),
                         static_cast<unsigned long long>(ForwardingTable::UnarmedCount()));
        });
    }
}

uint32_t ForwardingTable::EstimateLiveObjects(RegionInfo* region, size_t regionSize)
{
    // zForwarding.inline.hpp:43-50 sizes from live *object* count. GetLiveByteCount
    // is bytes; liveBytes>>3 counts 8-byte words. When the live count is still
    // zero (lazy EnsureEntries), take the region's capacity so the table cannot
    // fill and spin (REPORT-fwdentries).
    const uint64_t liveBytes = region->GetLiveByteCount();
    uint64_t estimate = liveBytes >> ZForwarding::kAlignShift;
    if (estimate == 0) {
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
    g_membership.put(forwarding->start(), forwarding->size(), forwarding);
    g_entries.put(forwarding->start(), forwarding->size(), forwarding);
}

void ForwardingTable::remove(ZForwarding* forwarding)
{
    // zForwardingTable.inline.hpp:56-62 — membership only. Entries stay until
    // ClearEntries (zRelocationSet.cpp:91-96 arena recycle is a phase later).
    if (forwarding == nullptr || !Ready()) {
        return;
    }
    g_membership.put(forwarding->start(), forwarding->size(), nullptr);
}

ZForwarding* ForwardingTable::get(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    if (RelocationSetTxn::Enabled()) {
        RelocationSetTxn::Handle handle;
        return ReaderEntries(addr, handle);
    }
    ZForwarding* forwarding = g_membership.get(addr);
    if (forwarding != nullptr && !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
        return nullptr;
    }
    return forwarding;
}

void ForwardingTable::Insert(MAddress regionStart, size_t regionSize, RegionInfo* region)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    EnsureEntries(region);
    ZForwarding* forwarding = g_entries.get(regionStart);
    if (forwarding == nullptr) {
        return;
    }
    g_membership.put(regionStart, regionSize, forwarding);
}

void ForwardingTable::Remove(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    g_membership.put(regionStart, regionSize, nullptr);
}

void ForwardingTable::EnsureEntries(RegionInfo* region)
{
    if (!Ready() || region == nullptr) {
        return;
    }
    const MAddress start = region->GetRegionStart();
    const size_t regionSize = region->GetRegionSize();
    if (g_entries.index_for_offset(start) == SIZE_MAX) {
        return;
    }
    if (g_entries.get(start) != nullptr) {
        return;
    }
    const uint32_t liveObjs = EstimateLiveObjects(region, regionSize);
    const RegionLifeId life = region->GetRegionLifeId();
    ZForwarding* created = ZForwarding::alloc(liveObjs, start, g_entries.base(), regionSize, region, life);
    if (created == nullptr) {
        return;
    }
    ZForwarding* expected = nullptr;
    if (!g_entries.compare_exchange(start, expected, created)) {
        created->Destroy();
        if (expected == nullptr || expected->start() != start) {
            return;
        }
        created = expected;
    }
    g_entries.put(start, regionSize, created);
    RegionLifeClock::Publish(RegionLifeClock::Carrier::ARMED_ENTRY, created->page_life_id());
}

void ForwardingTable::ClearEntries(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    ZForwarding* tab = g_entries.exchange(regionStart, nullptr);
    const size_t granule = g_entries.granule();
    if (granule != 0) {
        const size_t first = g_entries.index_for_offset(regionStart);
        const size_t count = regionSize / granule + ((regionSize % granule) != 0 ? 1 : 0);
        for (size_t i = 1; i < count && first != SIZE_MAX && first + i < g_entries.size(); ++i) {
            g_entries.put(g_entries.base() + (first + i) * granule, nullptr);
        }
    }
    if (tab != nullptr && tab->start() == regionStart) {
        Retire(tab);
    }
}

void ForwardingTable::RetireEnvelope(ZForwarding* envelope)
{
    if (!Ready() || envelope == nullptr) {
        return;
    }
    ZForwarding* expected = envelope;
    if (!g_entries.compare_exchange(envelope->start(), expected, nullptr)) {
        return;
    }
    const size_t granule = g_entries.granule();
    const size_t first = g_entries.index_for_offset(envelope->start());
    const size_t count = granule == 0 ? 0
        : envelope->size() / granule + ((envelope->size() % granule) != 0 ? 1 : 0);
    for (size_t i = 1; i < count && first != SIZE_MAX && first + i < g_entries.size(); ++i) {
        expected = envelope;
        (void)g_entries.compare_exchange(g_entries.base() + (first + i) * granule,
                                         expected, nullptr);
    }
    for (size_t i = 0; i < count && first != SIZE_MAX && first + i < g_membership.size(); ++i) {
        expected = envelope;
        (void)g_membership.compare_exchange(g_membership.base() + (first + i) * granule,
                                            expected, nullptr);
    }
    Retire(envelope);
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
    if (g_membership.get(tab->start()) == tab) {
        g_membership.put(tab->start(), tab->size(), nullptr);
    }
    if (g_entries.get(tab->start()) == tab) {
        g_entries.put(tab->start(), tab->size(), nullptr);
    }
    tab->Destroy();
}

namespace {
std::mutex g_retiredLock;
std::vector<ZForwarding*> g_retired;
std::vector<ZForwarding*> g_retiredAged;
std::atomic<uint64_t> g_retiredTotal{ 0 };
std::atomic<uint64_t> g_reclaimedTotal{ 0 };
std::atomic<uint64_t> g_retiredHeldPeak{ 0 };
} // namespace

void ForwardingTable::DropRetiredCovering(MAddress regionStart, size_t regionSize, bool detachPermit)
{
    if (regionSize == 0 || (RelocationSetTxn::Enabled() && !detachPermit)) {
        return;
    }
    const MAddress regionEnd = regionStart + regionSize;
    std::vector<ZForwarding*> victims;
    {
        std::lock_guard<std::mutex> lock(g_retiredLock);
        auto drop = [&](std::vector<ZForwarding*>& gens) {
            std::vector<ZForwarding*> keep;
            keep.reserve(gens.size());
            for (ZForwarding* tab : gens) {
                const MAddress tabStart = tab == nullptr ? 0 : tab->start();
                const MAddress tabEnd = tab == nullptr ? 0 : tabStart + tab->size();
                if (tab != nullptr && tabStart < regionEnd && regionStart < tabEnd) {
                    victims.push_back(tab);
                } else {
                    keep.push_back(tab);
                }
            }
            gens.swap(keep);
        };
        drop(g_retired);
        drop(g_retiredAged);
    }
    for (ZForwarding* tab : victims) {
        UnlinkThenDestroy(tab);
    }
}

void ForwardingTable::Retire(ZForwarding* tab)
{
    if (tab == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_retiredLock);
    g_retired.push_back(tab);
    RegionLifeClock::Publish(RegionLifeClock::Carrier::RETIRED_ENTRY, tab->page_life_id());
    g_retiredTotal.fetch_add(1, std::memory_order_relaxed);
}

void ForwardingTable::ReclaimRetired(const char* why)
{
    if (RelocationSetTxn::Enabled()) {
        size_t heldNow = 0;
        {
            std::lock_guard<std::mutex> lock(g_retiredLock);
            heldNow = g_retired.size() + g_retiredAged.size();
        }
        uint64_t peak = g_retiredHeldPeak.load(std::memory_order_relaxed);
        while (heldNow > peak &&
               !g_retiredHeldPeak.compare_exchange_weak(peak, heldNow, std::memory_order_relaxed)) {
        }
        if (heldNow != 0) {
            LOG(RTLOG_ERROR,
                "[FWDTABLE][reclaim] why=%s txn_authority=1 legacy_would_reclaim=%zu held_peak=%lu",
                why == nullptr ? "?" : why, heldNow,
                g_retiredHeldPeak.load(std::memory_order_relaxed));
        }
        return;
    }
    std::vector<ZForwarding*> victims;
    size_t heldNow = 0;
    {
        std::lock_guard<std::mutex> lock(g_retiredLock);
        for (ZForwarding* tab : g_retired) {
            RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::RETIRED_ENTRY,
                                                    tab != nullptr,
                                                    tab == nullptr ? 0 : tab->page_life_id());
        }
        for (ZForwarding* tab : g_retiredAged) {
            RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::RETIRED_ENTRY,
                                                    tab != nullptr,
                                                    tab == nullptr ? 0 : tab->page_life_id());
        }
        victims.swap(g_retiredAged);
        g_retiredAged.swap(g_retired);
        heldNow = g_retiredAged.size();
    }
    uint64_t peak = g_retiredHeldPeak.load(std::memory_order_relaxed);
    while (heldNow > peak && !g_retiredHeldPeak.compare_exchange_weak(peak, heldNow, std::memory_order_relaxed)) {
    }
    for (ZForwarding* tab : victims) {
        UnlinkThenDestroy(tab);
    }
    if (!victims.empty() || heldNow != 0) {
        const uint64_t done = g_reclaimedTotal.fetch_add(victims.size(), std::memory_order_relaxed) + victims.size();
        LOG(RTLOG_ERROR,
            "[FWDTABLE][reclaim] why=%s freed=%zu aged_held=%zu held_peak=%lu retired_total=%lu reclaimed_total=%lu",
            why == nullptr ? "?" : why, victims.size(), heldNow, g_retiredHeldPeak.load(std::memory_order_relaxed),
            g_retiredTotal.load(std::memory_order_relaxed), done);
    }
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
    return covers(g_retired) || covers(g_retiredAged);
}

ZForwarding* ForwardingTable::GetEntries(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    RelocationSetTxn::Handle handle;
    return ReaderEntries(addr, handle);
}

ZForwarding* ForwardingTable::GetEntriesForInstall(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    ZForwarding* forwarding = g_entries.get(addr);
    if (forwarding != nullptr && !forwarding->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY)) {
        return nullptr;
    }
    return forwarding;
}

bool ZForwarding::page_life_current(RegionLifeClock::Carrier carrier) const
{
    if (_page == nullptr) {
        RegionLifeClock::NoteUntracked(carrier);
        return true;
    }
    return RegionLifeClock::Validate(carrier, _page_life_id, _page->GetRegionLifeId());
}

MAddress ForwardingTable::InsertMapping(MAddress from, MAddress to)
{
    RelocationSetTxn::Handle txnHandle;
    ZForwarding* tab = ReaderEntries(from, txnHandle);
    if (tab == nullptr) {
        if (RelocationSetTxn::Enabled()) {
            return 0;
        }
        RegionInfo* region = nullptr;
        ZForwarding* membership = get(from);
        if (membership != nullptr) {
            region = membership->page();
        }
        if (region == nullptr) {
            region = RegionInfo::TryGetRegionInfoAt(from);
        }
        EnsureEntries(region);
        tab = ReaderEntries(from, txnHandle);
    }
    if (tab == nullptr) {
        return 0;
    }
    const MAddress stored = tab->insert(from, to);
    if (stored != 0) {
        tab->note_to_life(stored);
    }
    return stored;
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
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr) {
        return;
    }
    const MAddress start = toRegion->GetRegionStart();
    const uint8_t seq = toRegion->GetRegionLifeSeq();
    const RegionLifeId life = toRegion->GetRegionLifeId();
    for (uint8_t i = 0; i < _to_life_n; ++i) {
        if (_to_lives[i].start == start) {
            return;
        }
    }
    if (_to_life_n < 3) {
        _to_lives[_to_life_n].start = start;
        _to_lives[_to_life_n].legacySeq = seq;
        _to_lives[_to_life_n].lifeId = life;
        ++_to_life_n;
        RegionLifeClock::Publish(RegionLifeClock::Carrier::RECEIPT, life);
    } else {
        RegionLifeClock::NoteCapWouldOverflow(RegionLifeClock::Carrier::RECEIPT);
    }
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
    if (to == 0 || !Heap::IsHeapAddress(to)) {
        return 0;
    }
    BaseObject* obj = reinterpret_cast<BaseObject*>(to);
    if (!obj->IsValidObject()) {
        return 0;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    if (toRegion == nullptr || toRegion->IsFreeRegion() || toRegion->IsGarbageRegion()) {
        return 0;
    }
    if (to < toRegion->GetRegionStart() || to >= toRegion->GetRegionAllocPtr()) {
        return 0;
    }
    if (_to_life_n != 0) {
        const MAddress start = toRegion->GetRegionStart();
        const uint8_t seq = toRegion->GetRegionLifeSeq();
        const RegionLifeId life = toRegion->GetRegionLifeId();
        bool tracked = false;
        for (uint8_t i = 0; i < _to_life_n; ++i) {
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
    if (DestUsable(to)) {
        return to;
    }
    RelocationSetTxn::Handle txnHandle;
    ZForwarding* next = ReaderEntries(to, txnHandle);
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

static MAddress FindRetiredTo(MAddress from)
{
    if (RelocationSetTxn::Enabled()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_retiredLock);
    auto scan = [&](const std::vector<ZForwarding*>& gens) -> MAddress {
        for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
            ZForwarding* tab = *it;
            // A retired generation can outlive the address space which owned an unrelated table.
            // Geometry is self-contained in ZForwarding, so reject non-candidates before touching
            // the page pointer.  For a covering candidate the incarnation check still happens
            // before reading its forwarding payload, which is the required fail-closed order.
            if (tab != nullptr && tab->covers(from) &&
                tab->page_life_current(RegionLifeClock::Carrier::RETIRED_ENTRY)) {
                const MAddress to = tab->find(from);
                if (to != 0) {
                    return to;
                }
            }
        }
        return 0;
    };
    const MAddress fresh = scan(g_retired);
    if (fresh != 0) {
        return fresh;
    }
    return scan(g_retiredAged);
}

MAddress ForwardingTable::FindTo(MAddress from)
{
    RelocationSetTxn::Handle txnHandle;
    ZForwarding* tab = ReaderEntries(from, txnHandle);
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
    return FindRetiredTo(from);
}

bool ForwardingTable::EntriesArmed(MAddress from) { return GetEntries(from) != nullptr; }

MAddress ForwardingTable::LookupTo(MAddress from, ToAnswer* answer)
{
    RelocationSetTxn::Handle txnHandle;
    ZForwarding* tab = ReaderEntries(from, txnHandle);
    if (tab != nullptr) {
        const MAddress to = tab->find(from);
        if (to != 0) {
            g_armedHit.fetch_add(1, std::memory_order_relaxed);
            if (answer != nullptr) {
                *answer = ToAnswer::ArmedHit;
            }
            return to;
        }
    }
    const MAddress retired = FindRetiredTo(from);
    if (retired != 0) {
        g_armedHit.fetch_add(1, std::memory_order_relaxed);
        if (answer != nullptr) {
            *answer = ToAnswer::ArmedHit;
        }
        return retired;
    }
    if (tab != nullptr) {
        g_armedMiss.fetch_add(1, std::memory_order_relaxed);
        if (answer != nullptr) {
            *answer = ToAnswer::ArmedMiss;
        }
        return 0;
    }
    g_unarmed.fetch_add(1, std::memory_order_relaxed);
    if (answer != nullptr) {
        *answer = ToAnswer::Unarmed;
    }
    return 0;
}

uint64_t ForwardingTable::ArmedHitCount() { return g_armedHit.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::ArmedMissCount() { return g_armedMiss.load(std::memory_order_relaxed); }
uint64_t ForwardingTable::UnarmedCount() { return g_unarmed.load(std::memory_order_relaxed); }

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
