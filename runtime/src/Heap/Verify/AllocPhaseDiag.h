// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ALLOC_PHASE_DIAG_H
#define MRT_ALLOC_PHASE_DIAG_H

// marklate: record mutator GC phase at allocation for null-route samples.
// Gate: MRT_GCV2_NULLROUTE_DIAG=1 (default off). No TLS.
//
// Design: per-region *last allocation* stamp (not every object). Target shape from
// deadedge is region-end last object — a full object table is thrased under ALOT.

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
namespace AllocPhaseDiag {

// ~16k region slots; overwrite on collision (diag only).
constexpr size_t kCap = 1u << 14;
constexpr size_t kMask = kCap - 1;

struct RegionLast {
    std::atomic<uintptr_t> regionStart; // 0 = empty
    std::atomic<uintptr_t> lastObj;
    std::atomic<uint8_t> mutatorPhase;
    std::atomic<uint8_t> heapPhase;
    std::atomic<uint32_t> seq; // monotonic bump per update
};

inline RegionLast g_table[kCap] = {};
inline std::atomic<size_t> g_records{ 0 };

inline bool Enabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_NULLROUTE_DIAG");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

inline const char* PhaseName(uint8_t p)
{
    switch (static_cast<GCPhase>(p)) {
        case GC_PHASE_UNDEF:
            return "UNDEF";
        case GC_PHASE_IDLE:
            return "IDLE";
        case GC_PHASE_FINISH:
            return "FINISH";
        case GC_PHASE_RECLAIM_SATB_NODE:
            return "RECLAIM_SATB";
        case GC_PHASE_INIT:
            return "INIT";
        case GC_PHASE_ENUM:
            return "ENUM";
        case GC_PHASE_TRACE:
            return "TRACE";
        case GC_PHASE_CLEAR_SATB_BUFFER:
            return "CLEAR_SATB";
        case GC_PHASE_POST_TRACE:
            return "POST_TRACE";
        case GC_PHASE_PREFORWARD:
            return "PREFORWARD";
        case GC_PHASE_FORWARD:
            return "FORWARD";
        default:
            return "OTHER";
    }
}

inline bool IsMarkNewPhase(uint8_t p)
{
    return p == static_cast<uint8_t>(GC_PHASE_ENUM) || p == static_cast<uint8_t>(GC_PHASE_TRACE) ||
        p == static_cast<uint8_t>(GC_PHASE_CLEAR_SATB_BUFFER);
}

inline size_t HashRegion(uintptr_t regionStart)
{
    uintptr_t x = regionStart >> 16; // region typically 64KiB
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(x) & kMask;
}

// regionStart: base of the region; obj: allocated object address.
inline void Record(void* obj, uintptr_t regionStart, uint8_t mutatorPhase, uint8_t heapPhase)
{
    if (obj == nullptr || regionStart == 0 || !Enabled()) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t i = HashRegion(regionStart);
    for (size_t n = 0; n < 8; ++n) {
        size_t idx = (i + n) & kMask;
        uintptr_t expected = 0;
        RegionLast& e = g_table[idx];
        if (e.regionStart.compare_exchange_strong(expected, regionStart, std::memory_order_release,
                                                  std::memory_order_relaxed) ||
            expected == regionStart) {
            e.lastObj.store(addr, std::memory_order_release);
            e.mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
            e.heapPhase.store(heapPhase, std::memory_order_relaxed);
            e.seq.fetch_add(1, std::memory_order_relaxed);
            g_records.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // probe exhausted: overwrite home
    RegionLast& e = g_table[i];
    e.regionStart.store(regionStart, std::memory_order_release);
    e.lastObj.store(addr, std::memory_order_release);
    e.mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
    e.heapPhase.store(heapPhase, std::memory_order_relaxed);
    e.seq.fetch_add(1, std::memory_order_relaxed);
}

struct Lookup {
    bool found = false;
    bool isRegionLast = false;
    uint8_t mutatorPhase = 0;
    uint8_t heapPhase = 0;
    uintptr_t lastObj = 0;
};

// Prefer exact match against region's lastObj (target is region-end last alloc).
inline Lookup Find(void* obj, uintptr_t regionStart)
{
    Lookup r;
    if (obj == nullptr || !Enabled()) {
        return r;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t i = HashRegion(regionStart);
    for (size_t n = 0; n < 8; ++n) {
        size_t idx = (i + n) & kMask;
        RegionLast& e = g_table[idx];
        if (e.regionStart.load(std::memory_order_acquire) != regionStart) {
            continue;
        }
        r.lastObj = e.lastObj.load(std::memory_order_acquire);
        r.mutatorPhase = e.mutatorPhase.load(std::memory_order_relaxed);
        r.heapPhase = e.heapPhase.load(std::memory_order_relaxed);
        r.found = true;
        r.isRegionLast = (r.lastObj == addr);
        return r;
    }
    return r;
}

} // namespace AllocPhaseDiag
} // namespace MapleRuntime

#endif // MRT_ALLOC_PHASE_DIAG_H
