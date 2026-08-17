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
// Live stamp: monotonic last-obj within a region generation (addr only moves up).
// Freeze at PrepareForwardableRegion snapshots live → frozen, then clears live so
// the next generation on the same physical region starts clean.
// Near-end ring: last ~32 near-end (offset high) objects keep exact (obj→phase).

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
namespace AllocPhaseDiag {

constexpr size_t kCap = 1u << 14;
constexpr size_t kMask = kCap - 1;
constexpr size_t kNearCap = 1u << 14;
constexpr size_t kNearMask = kNearCap - 1;
// Objects within this many bytes of region end are also keyed by exact address.
constexpr size_t kNearEndBytes = 256;

struct Slot {
    std::atomic<uintptr_t> regionStart{ 0 };
    std::atomic<uintptr_t> lastObj{ 0 };
    std::atomic<uint8_t> mutatorPhase{ 0 };
    std::atomic<uint8_t> heapPhase{ 0 };
    std::atomic<uint8_t> isTraceAtLast{ 0 };
    std::atomic<uintptr_t> frozenLastObj{ 0 };
    std::atomic<uint8_t> frozenMut{ 0 };
    std::atomic<uint8_t> frozenHeap{ 0 };
    std::atomic<uint8_t> frozenIsTrace{ 0 };
    std::atomic<uint8_t> frozenValid{ 0 };
    // blackmark: SetTraceRegionFlag(1→0) count for this physical region generation.
    std::atomic<uint32_t> clearTraceCnt{ 0 };
    std::atomic<uint8_t> everWasTrace{ 0 };
};

struct NearEntry {
    std::atomic<uintptr_t> obj{ 0 };
    std::atomic<uint8_t> mutatorPhase{ 0 };
    std::atomic<uint8_t> heapPhase{ 0 };
    std::atomic<uint8_t> isTraceAtAlloc{ 0 };
};

inline Slot g_table[kCap] = {};
inline NearEntry g_near[kNearCap] = {};

inline bool Enabled()
{
    return false;
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

inline size_t HashObj(uintptr_t obj)
{
    uintptr_t x = obj >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b97f4a7c15ull;
    return static_cast<size_t>(x) & kNearMask;
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

inline void RecordNear(uintptr_t obj, uint8_t mutP, uint8_t heapP, uint8_t isTrace)
{
    size_t i = HashObj(obj);
    for (size_t n = 0; n < 4; ++n) {
        size_t idx = (i + n) & kNearMask;
        NearEntry& e = g_near[idx];
        uintptr_t expected = 0;
        if (e.obj.compare_exchange_strong(expected, obj, std::memory_order_release, std::memory_order_relaxed) ||
            expected == obj || e.obj.load(std::memory_order_acquire) == obj) {
            e.obj.store(obj, std::memory_order_release);
            e.mutatorPhase.store(mutP, std::memory_order_relaxed);
            e.heapPhase.store(heapP, std::memory_order_relaxed);
            e.isTraceAtAlloc.store(isTrace, std::memory_order_relaxed);
            return;
        }
    }
    NearEntry& e = g_near[i];
    e.obj.store(obj, std::memory_order_release);
    e.mutatorPhase.store(mutP, std::memory_order_relaxed);
    e.heapPhase.store(heapP, std::memory_order_relaxed);
    e.isTraceAtAlloc.store(isTrace, std::memory_order_relaxed);
}

// regionEnd: exclusive end (regionStart + size). 0 if unknown.
inline void Record(void* obj, uintptr_t regionStart, uintptr_t regionEnd, uint8_t mutatorPhase, uint8_t heapPhase,
                   uint8_t isTraceRegion)
{
    if (obj == nullptr || regionStart == 0 || !Enabled()) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    Slot* e = FindSlot(regionStart, true);
    if (e != nullptr) {
        // Monotonic within generation: only advance lastObj forward.
        uintptr_t prev = e->lastObj.load(std::memory_order_relaxed);
        if (prev == 0 || addr >= prev) {
            e->lastObj.store(addr, std::memory_order_release);
            e->mutatorPhase.store(mutatorPhase, std::memory_order_relaxed);
            e->heapPhase.store(heapPhase, std::memory_order_relaxed);
            e->isTraceAtLast.store(isTraceRegion, std::memory_order_relaxed);
        }
        if (isTraceRegion != 0) {
            e->everWasTrace.store(1, std::memory_order_relaxed);
        }
    }
    if (regionEnd != 0 && regionEnd > addr && (regionEnd - addr) <= kNearEndBytes) {
        RecordNear(addr, mutatorPhase, heapPhase, isTraceRegion);
    }
}

inline void NoteTraceFlagCleared(uintptr_t regionStart)
{
    if (regionStart == 0 || !Enabled()) {
        return;
    }
    Slot* e = FindSlot(regionStart, true);
    if (e == nullptr) {
        return;
    }
    e->clearTraceCnt.fetch_add(1, std::memory_order_relaxed);
    e->everWasTrace.store(1, std::memory_order_relaxed);
}

inline void NoteTraceFlagSet(uintptr_t regionStart)
{
    if (regionStart == 0 || !Enabled()) {
        return;
    }
    Slot* e = FindSlot(regionStart, true);
    if (e == nullptr) {
        return;
    }
    e->everWasTrace.store(1, std::memory_order_relaxed);
}

inline void FreezeRegion(uintptr_t regionStart)
{
    if (regionStart == 0 || !Enabled()) {
        return;
    }
    Slot* e = FindSlot(regionStart, true);
    if (e == nullptr) {
        return;
    }
    e->frozenLastObj.store(e->lastObj.load(std::memory_order_acquire), std::memory_order_release);
    e->frozenMut.store(e->mutatorPhase.load(std::memory_order_relaxed), std::memory_order_relaxed);
    e->frozenHeap.store(e->heapPhase.load(std::memory_order_relaxed), std::memory_order_relaxed);
    e->frozenIsTrace.store(e->isTraceAtLast.load(std::memory_order_relaxed), std::memory_order_relaxed);
    e->frozenValid.store(1, std::memory_order_release);
    // Clear live so next generation on this region starts clean.
    e->lastObj.store(0, std::memory_order_release);
    e->mutatorPhase.store(0, std::memory_order_relaxed);
    e->heapPhase.store(0, std::memory_order_relaxed);
    e->isTraceAtLast.store(0, std::memory_order_relaxed);
}

struct Lookup {
    bool found = false;
    bool isRegionLast = false;
    bool usedFrozen = false;
    bool usedNear = false;
    uint8_t mutatorPhase = 0;
    uint8_t heapPhase = 0;
    uint8_t isTraceAtAlloc = 0;
    uint32_t clearTraceCnt = 0;
    uint8_t everWasTrace = 0;
    uintptr_t lastObj = 0;
};

inline Lookup FindNear(uintptr_t obj)
{
    Lookup r;
    size_t i = HashObj(obj);
    for (size_t n = 0; n < 4; ++n) {
        size_t idx = (i + n) & kNearMask;
        NearEntry& e = g_near[idx];
        if (e.obj.load(std::memory_order_acquire) == obj) {
            r.found = true;
            r.usedNear = true;
            r.isRegionLast = true; // exact near-end hit
            r.mutatorPhase = e.mutatorPhase.load(std::memory_order_relaxed);
            r.heapPhase = e.heapPhase.load(std::memory_order_relaxed);
            r.isTraceAtAlloc = e.isTraceAtAlloc.load(std::memory_order_relaxed);
            r.lastObj = obj;
            return r;
        }
    }
    return r;
}

inline Lookup Find(void* obj, uintptr_t regionStart)
{
    Lookup r;
    if (obj == nullptr || !Enabled()) {
        return r;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    // Prefer exact near-end table (survives freeze clear of live lastObj).
    Lookup nearLookup = FindNear(addr);
    Slot* e = FindSlot(regionStart, false);
    if (e != nullptr) {
        r.clearTraceCnt = e->clearTraceCnt.load(std::memory_order_relaxed);
        r.everWasTrace = e->everWasTrace.load(std::memory_order_relaxed);
    }
    if (nearLookup.found) {
        nearLookup.clearTraceCnt = r.clearTraceCnt;
        nearLookup.everWasTrace = r.everWasTrace;
        return nearLookup;
    }
    if (e == nullptr) {
        return r;
    }
    if (e->frozenValid.load(std::memory_order_acquire) != 0) {
        r.usedFrozen = true;
        r.lastObj = e->frozenLastObj.load(std::memory_order_acquire);
        r.mutatorPhase = e->frozenMut.load(std::memory_order_relaxed);
        r.heapPhase = e->frozenHeap.load(std::memory_order_relaxed);
        r.isTraceAtAlloc = e->frozenIsTrace.load(std::memory_order_relaxed);
        r.found = (r.lastObj != 0);
        r.isRegionLast = (r.lastObj == addr);
        return r;
    }
    r.lastObj = e->lastObj.load(std::memory_order_acquire);
    r.mutatorPhase = e->mutatorPhase.load(std::memory_order_relaxed);
    r.heapPhase = e->heapPhase.load(std::memory_order_relaxed);
    r.isTraceAtAlloc = e->isTraceAtLast.load(std::memory_order_relaxed);
    r.found = (r.lastObj != 0);
    r.isRegionLast = (r.lastObj == addr);
    return r;
}

} // namespace AllocPhaseDiag
} // namespace MapleRuntime

#endif // MRT_ALLOC_PHASE_DIAG_H
