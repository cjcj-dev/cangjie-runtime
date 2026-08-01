// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TIME_WINDOW_EVAC_RING_H
#define MRT_TIME_WINDOW_EVAC_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
// timewindow measure: durable set of CopyObject from-addresses.
// Observation only — never consulted to rewrite references.
// Lifetime: entries persist across Clear of ForwardFactTable so a later fault
// can still ask "was this address evacuated this process?".
//
// Two layers:
//   (1) open-addressing hash of exact from-base (never overwrites different keys
//       until full; CAP=16M covers multi-million copy stress loads).
//   (2) ring of (from,to,size,preState) for detail on recent copies (interior match).
class TimeWindowEvacRing {
public:
    static constexpr size_t RING_CAP = 1u << 22;  // 4M detail slots
    static constexpr size_t HASH_CAP = 1u << 24;  // 16M exact-from slots

    struct Entry {
        uintptr_t from = 0;
        uintptr_t to = 0;
        uint32_t size = 0;
        uint32_t preState = 0; // ObjectState at Record entry (from header)
    };

    static TimeWindowEvacRing& Instance() noexcept;

    // Called from CopyObject after payload commit. No locks.
    void Record(BaseObject* from, BaseObject* to, size_t size) noexcept;

    // Best-effort (signal path). Exact from-base via hash; interior via ring.
    // Returns true if any match; fills *out when detail is available.
    bool Lookup(uintptr_t addr, Entry* out) const noexcept;

    uint64_t TotalRecorded() const noexcept { return total_.load(std::memory_order_relaxed); }
    uint64_t HashInserts() const noexcept { return hashInserts_.load(std::memory_order_relaxed); }
    uint64_t HashFullDrops() const noexcept { return hashFullDrops_.load(std::memory_order_relaxed); }

private:
    TimeWindowEvacRing() = default;
    ~TimeWindowEvacRing() = default;
    TimeWindowEvacRing(const TimeWindowEvacRing&) = delete;
    TimeWindowEvacRing& operator=(const TimeWindowEvacRing&) = delete;

    std::atomic<uint64_t> seq_{ 0 };
    std::atomic<uint64_t> total_{ 0 };
    std::atomic<uint64_t> hashInserts_{ 0 };
    std::atomic<uint64_t> hashFullDrops_{ 0 };
    Entry slots_[RING_CAP];
    // 0 = empty; else from-base pointer. Never cleared for process lifetime.
    std::atomic<uintptr_t> fromHash_[HASH_CAP];
};

// AS-safe dump for signal path: look up si_addr (and its low-48) in the ring.
void PrintTimeWindowEvacLookup(const void* siAddr) noexcept;
} // namespace MapleRuntime

#endif // MRT_TIME_WINDOW_EVAC_RING_H
