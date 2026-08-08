// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ALLOC_PHASE_DIAG_H
#define MRT_ALLOC_PHASE_DIAG_H

// marklate: record mutator GC phase at allocation for null-route samples.
// Gate: MRT_GCV2_NULLROUTE_DIAG=1 (default off). No TLS — open hash table only.
// Lookup is best-effort (capacity-bounded; oldest entries may be overwritten).

#include <atomic>
#include <cstdint>
#include <cstring>

#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
namespace AllocPhaseDiag {

// Fixed power-of-two capacity; overwrite on collision (diag only).
constexpr size_t kCap = 1u << 16; // 65536 slots
constexpr size_t kMask = kCap - 1;

struct Entry {
    std::atomic<uintptr_t> addr; // 0 = empty
    std::atomic<uint8_t> mutatorPhase;
    std::atomic<uint8_t> heapPhase;
};

inline Entry g_table[kCap] = {};
inline std::atomic<size_t> g_records{ 0 };
inline std::atomic<size_t> g_overwrites{ 0 };

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

// True if phase is one of the three MarkNewObject arms.
inline bool IsMarkNewPhase(uint8_t p)
{
    return p == static_cast<uint8_t>(GC_PHASE_ENUM) || p == static_cast<uint8_t>(GC_PHASE_TRACE) ||
        p == static_cast<uint8_t>(GC_PHASE_CLEAR_SATB_BUFFER);
}

inline size_t Hash(uintptr_t addr)
{
    // Mix low bits; objects are 8/16-aligned.
    uintptr_t x = addr >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(x) & kMask;
}

inline void Record(void* obj, uint8_t mutatorPhase, uint8_t heapPhase)
{
    if (obj == nullptr || !Enabled()) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t i = Hash(addr);
    for (size_t n = 0; n < 8; ++n) {
        size_t idx = (i + n) & kMask;
        uintptr_t expected = 0;
        if (g_table[idx].addr.compare_exchange_strong(expected, addr, std::memory_order_release,
                                                      std::memory_order_relaxed)) {
            g_table[idx].mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
            g_table[idx].heapPhase.store(heapPhase, std::memory_order_relaxed);
            g_records.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (expected == addr) {
            g_table[idx].mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
            g_table[idx].heapPhase.store(heapPhase, std::memory_order_relaxed);
            return;
        }
    }
    // Probe exhausted: overwrite home slot.
    size_t idx = i;
    g_table[idx].addr.store(addr, std::memory_order_release);
    g_table[idx].mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
    g_table[idx].heapPhase.store(heapPhase, std::memory_order_relaxed);
    g_overwrites.fetch_add(1, std::memory_order_relaxed);
}

struct Lookup {
    bool found = false;
    uint8_t mutatorPhase = 0;
    uint8_t heapPhase = 0;
};

inline Lookup Find(void* obj)
{
    Lookup r;
    if (obj == nullptr || !Enabled()) {
        return r;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t i = Hash(addr);
    for (size_t n = 0; n < 8; ++n) {
        size_t idx = (i + n) & kMask;
        if (g_table[idx].addr.load(std::memory_order_acquire) == addr) {
            r.found = true;
            r.mutatorPhase = g_table[idx].mutatorPhase.load(std::memory_order_relaxed);
            r.heapPhase = g_table[idx].heapPhase.load(std::memory_order_relaxed);
            return r;
        }
    }
    return r;
}

} // namespace AllocPhaseDiag
} // namespace MapleRuntime

#endif // MRT_ALLOC_PHASE_DIAG_H
