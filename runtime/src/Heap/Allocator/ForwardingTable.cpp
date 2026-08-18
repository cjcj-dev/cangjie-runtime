// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/ForwardingTable.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace {

struct FwdSlot {
    std::atomic<RegionInfo*> region;
    std::atomic<ForwardingEntries*> entries;
};

FwdSlot* g_map = nullptr;
size_t g_entries = 0;
MAddress g_base = 0;
size_t g_unitSize = 0;
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

} // namespace

bool ForwardingTable::Ready() { return g_ready.load(std::memory_order_acquire); }

size_t ForwardingTable::IndexFor(MAddress addr)
{
    if (addr < g_base) {
        return SIZE_MAX;
    }
    const size_t idx = static_cast<size_t>(addr - g_base) / g_unitSize;
    return idx < g_entries ? idx : SIZE_MAX;
}

void ForwardingTable::Initialize(MAddress heapStart, size_t heapSize, size_t unitSize)
{
    if (g_ready.load(std::memory_order_acquire) || unitSize == 0 || heapSize == 0) {
        return;
    }
    const size_t entries = heapSize / unitSize + 1;
    auto* map = static_cast<FwdSlot*>(std::calloc(entries, sizeof(FwdSlot)));
    if (map == nullptr) {
        LOG(RTLOG_ERROR, "[FWDTABLE] calloc failed entries=%zu -- table stays off", entries);
        return;
    }
    g_map = map;
    g_entries = entries;
    g_base = heapStart;
    g_unitSize = unitSize;
    g_ready.store(true, std::memory_order_release);
    LOG(RTLOG_ERROR, "[FWDTABLE] armed base=%#zx size=%zu unit=%zu entries=%zu", static_cast<size_t>(heapStart),
        heapSize, unitSize, entries);
}

void ForwardingTable::Insert(MAddress regionStart, size_t regionSize, RegionInfo* region)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    const size_t first = IndexFor(regionStart);
    if (first == SIZE_MAX) {
        return;
    }
    const size_t count = regionSize / g_unitSize + ((regionSize % g_unitSize) != 0 ? 1 : 0);
    for (size_t i = 0; i < count && first + i < g_entries; ++i) {
        g_map[first + i].region.store(region, std::memory_order_release);
    }
}

void ForwardingTable::Remove(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    const size_t first = IndexFor(regionStart);
    if (first == SIZE_MAX) {
        return;
    }
    const size_t count = regionSize / g_unitSize + ((regionSize % g_unitSize) != 0 ? 1 : 0);
    for (size_t i = 0; i < count && first + i < g_entries; ++i) {
        g_map[first + i].region.store(nullptr, std::memory_order_release);
    }
}

void ForwardingTable::EnsureEntries(RegionInfo* region)
{
    if (!Ready() || region == nullptr) {
        return;
    }
    const MAddress start = region->GetRegionStart();
    const size_t regionSize = region->GetRegionSize();
    const size_t first = IndexFor(start);
    if (first == SIZE_MAX) {
        return;
    }
    if (g_map[first].entries.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    // ZGC sizes the table from the live *object* count and doubles it, so the load factor stays
    // under one half and the open-addressing probe always terminates (zForwarding.inline.hpp:43-50).
    //
    // Two things went wrong with copying that here. liveBytes >> kAlignShift counts eight-byte
    // words, not objects, so it is not the same quantity. And this runs lazily, on the first insert
    // into a region, at which point GetLiveByteCount() can still be zero -- which used to fall
    // through to liveObjs = 1 and a table with two slots. The third object forwarded out of that
    // region then probed a full table forever: two GC threads pinned at 100% CPU inside find(),
    // and a workload that takes ten seconds ran past a five-minute timeout.
    //
    // The probes are bounded now, so a table that fills degrades to a geometry fallback rather than
    // a hang. This still sizes it not to fill: when the live count is unusable, take the region's
    // own capacity as the bound on how many objects can possibly be in it.
    const uint64_t liveBytes = region->GetLiveByteCount();
    uint64_t estimate = liveBytes >> ForwardingEntries::kAlignShift;
    if (estimate == 0) {
        estimate = regionSize >> ForwardingEntries::kAlignShift;
    }
    if (estimate == 0) {
        estimate = 1;
    }
    const uint32_t liveObjs = static_cast<uint32_t>(std::min<uint64_t>(estimate, UINT32_MAX));
    ForwardingEntries* created = ForwardingEntries::Create(liveObjs, start, g_base);
    if (created == nullptr) {
        return;
    }
    ForwardingEntries* expected = nullptr;
    if (!g_map[first].entries.compare_exchange_strong(expected, created, std::memory_order_release,
                                                      std::memory_order_acquire)) {
        created->Destroy();
        // Adopting whatever was already there is only right when it belongs to this region. The
        // fan-out below writes one pointer across every unit a region covers, so a slot can hold a
        // *neighbour's* table -- and inserting into that one would index from the wrong base and,
        // worse, hand this region an owner it does not own.
        if (expected == nullptr || expected->start() != start) {
            return;
        }
        created = expected;
    }
    const size_t count = regionSize / g_unitSize + ((regionSize % g_unitSize) != 0 ? 1 : 0);
    for (size_t i = 1; i < count && first + i < g_entries; ++i) {
        g_map[first + i].entries.store(created, std::memory_order_release);
    }
}

void ForwardingTable::ClearEntries(MAddress regionStart, size_t regionSize)
{
    if (!Ready() || regionSize == 0) {
        return;
    }
    const size_t first = IndexFor(regionStart);
    if (first == SIZE_MAX) {
        return;
    }
    ForwardingEntries* tab = g_map[first].entries.exchange(nullptr, std::memory_order_acq_rel);
    const size_t count = regionSize / g_unitSize + ((regionSize % g_unitSize) != 0 ? 1 : 0);
    for (size_t i = 1; i < count && first + i < g_entries; ++i) {
        g_map[first + i].entries.store(nullptr, std::memory_order_release);
    }
    // One table, many slots: EnsureEntries publishes the same pointer across every unit the region
    // covers, so "I took a non-null pointer out of a slot" is not "I own it". The table records the
    // address it was built for, so ownership is a comparison rather than a convention.
    //
    // Retired, not freed. ZGC's entries live in the ZForwardingAllocator arena and that arena is
    // recycled only by the *next* cycle's ZRelocationSetInstallTask (zRelocationSet.cpp:91-96);
    // ZHeap::free_page frees the page and nothing else (zRelocate.cpp:1041-1047), and
    // reset_relocation_set runs destructors only (zRelocationSet.cpp:191-196). Page death and table
    // death are a whole phase apart, and that gap is what licenses ZForwarding::find to run with no
    // reference held -- which it does, before retain_page rather than after (zRelocate.cpp:382-393).
    //
    // Ours had no gap: region reuse was the free. ClearEntries is reached from InitRegionInfo, so a
    // mutator allocating a fresh region called std::free on a table that FindToVersion was reading
    // -- and FindToVersion sits on the mutator read barrier whenever kStaleGuard is on, holding
    // nothing, gated only on IsHeapAddress. Deferring the free to a cycle boundary restores the gap
    // instead of asking every reader to take a reference.
    //
    // It also removes a read-after-free that did not need concurrency at all: a small region is 32
    // units, InitFreeUnits walks them one at a time, and each call landed here with count == 1.
    // Iteration 0 freed the table; iterations 1..31 then evaluated tab->start() on freed storage.
    if (tab != nullptr && tab->start() == regionStart) {
        Retire(tab);
    }
}

// Tables unlinked from the map but not yet freed, and the count of frees deferred through them.
// A pointer sitting here is unreachable through the map -- no new reader can find it -- but a reader
// that loaded it before the unlink may still be inside find().
namespace {
std::mutex g_retiredLock;
std::vector<ForwardingEntries*> g_retired;
std::atomic<uint64_t> g_retiredTotal{ 0 };
std::atomic<uint64_t> g_reclaimedTotal{ 0 };
} // namespace

void ForwardingTable::Retire(ForwardingEntries* tab)
{
    if (tab == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_retiredLock);
    g_retired.push_back(tab);
    g_retiredTotal.fetch_add(1, std::memory_order_relaxed);
}

void ForwardingTable::ReclaimRetired(const char* why)
{
    std::vector<ForwardingEntries*> victims;
    {
        std::lock_guard<std::mutex> lock(g_retiredLock);
        victims.swap(g_retired);
    }
    for (ForwardingEntries* tab : victims) {
        tab->Destroy();
    }
    if (!victims.empty()) {
        const uint64_t done = g_reclaimedTotal.fetch_add(victims.size(), std::memory_order_relaxed) +
            victims.size();
        LOG(RTLOG_ERROR, "[FWDTABLE][reclaim] why=%s freed=%zu retired_total=%lu reclaimed_total=%lu",
            why == nullptr ? "?" : why, victims.size(),
            g_retiredTotal.load(std::memory_order_relaxed), done);
    }
}

RegionInfo* ForwardingTable::Get(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    const size_t idx = IndexFor(addr);
    if (idx == SIZE_MAX) {
        return nullptr;
    }
    return g_map[idx].region.load(std::memory_order_acquire);
}

ForwardingEntries* ForwardingTable::GetEntries(MAddress addr)
{
    if (!Ready()) {
        return nullptr;
    }
    const size_t idx = IndexFor(addr);
    if (idx == SIZE_MAX) {
        return nullptr;
    }
    return g_map[idx].entries.load(std::memory_order_acquire);
}

MAddress ForwardingTable::InsertMapping(MAddress from, MAddress to)
{
    ForwardingEntries* tab = GetEntries(from);
    if (tab == nullptr) {
        RegionInfo* region = Get(from);
        if (region == nullptr) {
            region = RegionInfo::TryGetRegionInfoAt(from);
        }
        EnsureEntries(region);
        tab = GetEntries(from);
    }
    if (tab == nullptr) {
        return 0;
    }
    return tab->insert(from, to);
}

MAddress ForwardingTable::FindTo(MAddress from)
{
    ForwardingEntries* tab = GetEntries(from);
    if (tab == nullptr) {
        return 0;
    }
    return tab->find(from);
}

bool ForwardingTable::EntriesArmed(MAddress from) { return GetEntries(from) != nullptr; }

MAddress ForwardingTable::LookupTo(MAddress from, ToAnswer* answer)
{
    ForwardingEntries* tab = GetEntries(from);
    if (tab == nullptr) {
        g_unarmed.fetch_add(1, std::memory_order_relaxed);
        if (answer != nullptr) {
            *answer = ToAnswer::Unarmed;
        }
        return 0;
    }
    const MAddress to = tab->find(from);
    if (to != 0) {
        g_armedHit.fetch_add(1, std::memory_order_relaxed);
        if (answer != nullptr) {
            *answer = ToAnswer::ArmedHit;
        }
        return to;
    }
    g_armedMiss.fetch_add(1, std::memory_order_relaxed);
    if (answer != nullptr) {
        *answer = ToAnswer::ArmedMiss;
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
    const bool table = Get(addr) != nullptr;
    const uint64_t n = g_cmpTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (table == legacy) {
        g_cmpAgree.fetch_add(1, std::memory_order_relaxed);
    } else {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
        const unsigned rtype = region == nullptr ? kTypeBuckets - 1
                                                 : static_cast<unsigned>(region->GetRegionType());
        const unsigned bucket = rtype < kTypeBuckets ? rtype : kTypeBuckets - 1;
        if (table) {
            g_cmpTableOnly.fetch_add(1, std::memory_order_relaxed);
            g_tableOnlyByType[bucket].fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cmpLegacyOnly.fetch_add(1, std::memory_order_relaxed);
            g_legacyOnlyByType[bucket].fetch_add(1, std::memory_order_relaxed);
            const unsigned ghost = (region != nullptr && region->IsGhostFromRegion()) ? 1u : 0u;
            LOG(RTLOG_ERROR, "[FWDTABLE][legacyOnly] addr=%#zx rtype=%u ghost=%u", static_cast<size_t>(addr),
                rtype, ghost);
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
    LOG(RTLOG_ERROR, "[FWDENT][sole] why=%s armedHit=%lu armedMiss=%lu unarmed=%lu",
        why == nullptr ? "?" : why, g_armedHit.load(std::memory_order_relaxed),
        g_armedMiss.load(std::memory_order_relaxed), g_unarmed.load(std::memory_order_relaxed));
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
