// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/ForwardingTable.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace {

// ZGranuleMap is a flat array over the whole address range it covers, not a hash: one shift and one
// load, no probing, no allocation on the query path.  Same here, indexed by allocation unit.
std::atomic<RegionInfo*>* g_map = nullptr;
size_t g_entries = 0;
MAddress g_base = 0;
size_t g_unitSize = 0;
std::atomic<bool> g_ready{ false };

// Disagreement accounting for step 1.  Both directions matter and they mean different things:
//   tableOnly  the table says relocating, the region predicates do not
//   legacyOnly the region predicates say relocating, the table does not
// The second is the dangerous one -- it would mean the port loses information.
std::atomic<uint64_t> g_cmpTotal{ 0 };
std::atomic<uint64_t> g_cmpAgree{ 0 };
std::atomic<uint64_t> g_cmpTableOnly{ 0 };
std::atomic<uint64_t> g_cmpLegacyOnly{ 0 };
constexpr unsigned kTypeBuckets = 16;
std::atomic<uint64_t> g_tableOnlyByType[kTypeBuckets] = {};
std::atomic<uint64_t> g_legacyOnlyByType[kTypeBuckets] = {};

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
    auto* map = static_cast<std::atomic<RegionInfo*>*>(std::calloc(entries, sizeof(std::atomic<RegionInfo*>)));
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
        g_map[first + i].store(region, std::memory_order_release);
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
        g_map[first + i].store(nullptr, std::memory_order_release);
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
    return g_map[idx].load(std::memory_order_acquire);
}

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
        // Every disagreement has to be explainable by region type, or the port is not equivalent.
        // Counting by type rather than sampling: legacyOnly sits at exactly 6 per run, which a
        // powers-of-two sampler would never show, and the campaign has already drawn one wrong
        // population conclusion from a sampled head.
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
            // legacyOnly must reach zero; it is small enough to print every one.
            LOG(RTLOG_ERROR, "[FWDTABLE][legacyOnly] addr=%#zx rtype=%u ghost=%u", static_cast<size_t>(addr),
                rtype, ghost);
        }
    }
    // Emit the zero case too: an all-agree run and a table that never got populated look the same
    // otherwise, and that confusion has cost this campaign a turn before.
    if ((n & (n - 1)) == 0) {
        DumpCompare("periodic");
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
    for (unsigned t = 0; t < kTypeBuckets; ++t) {
        const uint64_t to = g_tableOnlyByType[t].load(std::memory_order_relaxed);
        const uint64_t lo = g_legacyOnlyByType[t].load(std::memory_order_relaxed);
        if (to != 0 || lo != 0) {
            LOG(RTLOG_ERROR, "[FWDTABLE][bytype] rtype=%u tableOnly=%lu legacyOnly=%lu", t, to, lo);
        }
    }
}
} // namespace MapleRuntime
