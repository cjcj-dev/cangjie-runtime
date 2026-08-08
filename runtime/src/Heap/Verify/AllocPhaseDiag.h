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
// Live stamp: per-region last allocation (monotonic).
// Frozen stamp: snapshot at PrepareForwardableRegion — survives region reuse so
// GetRoute on ghost FROM still sees the phase of the generation under evacuation.

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
namespace AllocPhaseDiag {

constexpr size_t kCap = 1u << 14;
constexpr size_t kMask = kCap - 1;

struct Slot {
    std::atomic<uintptr_t> regionStart{ 0 };
    // live (mutator updates)
    std::atomic<uintptr_t> lastObj{ 0 };
    std::atomic<uint8_t> mutatorPhase{ 0 };
    std::atomic<uint8_t> heapPhase{ 0 };
    // frozen at PrepareForwardableRegion
    std::atomic<uintptr_t> frozenLastObj{ 0 };
    std::atomic<uint8_t> frozenMut{ 0 };
    std::atomic<uint8_t> frozenHeap{ 0 };
    std::atomic<uint8_t> frozenValid{ 0 };
};

inline Slot g_table[kCap] = {};

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
    uintptr_t x = regionStart >> 16;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(x) & kMask;
}

inline Slot* FindSlot(uintptr_t regionStart, bool create)
{
    size_t i = HashRegion(regionStart);
    for (size_t n = 0; n < 8; ++n) {
        size_t idx = (i + n) & kMask;
        Slot& e = g_table[idx];
        uintptr_t expected = 0;
        if (e.regionStart.compare_exchange_strong(expected, regionStart, std::memory_order_release,
                                                  std::memory_order_relaxed) ||
            expected == regionStart || e.regionStart.load(std::memory_order_acquire) == regionStart) {
            return &e;
        }
        if (!create) {
            continue;
        }
    }
    if (!create) {
        return nullptr;
    }
    Slot& e = g_table[i];
    e.regionStart.store(regionStart, std::memory_order_release);
    return &e;
}

inline void Record(void* obj, uintptr_t regionStart, uint8_t mutatorPhase, uint8_t heapPhase)
{
    if (obj == nullptr || regionStart == 0 || !Enabled()) {
        return;
    }
    Slot* e = FindSlot(regionStart, true);
    if (e == nullptr) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    e->lastObj.store(addr, std::memory_order_release);
    e->mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
    e->heapPhase.store(heapPhase, std::memory_order_relaxed);
}

// Call from PrepareForwardableRegion: freeze live stamp for this generation.
inline void FreezeRegion(uintptr_t regionStart)
{
    if (regionStart == 0 || !Enabled()) {
        return;
    }
    Slot* e = FindSlot(regionStart, false);
    if (e == nullptr) {
        // still create empty frozen so Find reports found=0 clearly
        e = FindSlot(regionStart, true);
        if (e == nullptr) {
            return;
        }
        e->frozenValid.store(0, std::memory_order_release);
        return;
    }
    e->frozenLastObj.store(e->lastObj.load(std::memory_order_acquire), std::memory_order_release);
    e->frozenMut.store(e->mutatorPhase.load(std::memory_order_relaxed), std::memory_order_relaxed);
    e->frozenHeap.store(e->heapPhase.load(std::memory_order_relaxed), std::memory_order_relaxed);
    e->frozenValid.store(1, std::memory_order_release);
}

struct Lookup {
    bool found = false;
    bool isRegionLast = false;
    bool usedFrozen = false;
    uint8_t mutatorPhase = 0;
    uint8_t heapPhase = 0;
    uintptr_t lastObj = 0;
};

// Prefer frozen stamp (ghost generation); fall back to live.
inline Lookup Find(void* obj, uintptr_t regionStart)
{
    Lookup r;
    if (obj == nullptr || !Enabled()) {
        return r;
    }
    Slot* e = FindSlot(regionStart, false);
    if (e == nullptr) {
        return r;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (e->frozenValid.load(std::memory_order_acquire) != 0) {
        r.usedFrozen = true;
        r.lastObj = e->frozenLastObj.load(std::memory_order_acquire);
        r.mutatorPhase = e->frozenMut.load(std::memory_order_relaxed);
        r.heapPhase = e->frozenHeap.load(std::memory_order_relaxed);
        r.found = true;
        r.isRegionLast = (r.lastObj == addr);
        return r;
    }
    r.lastObj = e->lastObj.load(std::memory_order_acquire);
    r.mutatorPhase = e->mutatorPhase.load(std::memory_order_relaxed);
    r.heapPhase = e->heapPhase.load(std::memory_order_relaxed);
    r.found = (r.lastObj != 0);
    r.isRegionLast = (r.lastObj == addr);
    return r;
}

} // namespace AllocPhaseDiag
} // namespace MapleRuntime

#endif // MRT_ALLOC_PHASE_DIAG_H
