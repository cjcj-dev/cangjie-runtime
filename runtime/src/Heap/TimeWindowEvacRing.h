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
// timewindow measure: lock-free ring of CopyObject from-addresses.
// Observation only — never consulted to rewrite references.
// Lifetime: entries persist across Clear of ForwardFactTable so a later fault
// can still ask "was this address evacuated this process?".
class TimeWindowEvacRing {
public:
    static constexpr size_t CAP = 1u << 20; // 1M slots

    struct Entry {
        uintptr_t from = 0;
        uintptr_t to = 0;
        uint32_t size = 0;
        uint32_t preState = 0; // ObjectState at Record entry (from header)
    };

    static TimeWindowEvacRing& Instance() noexcept;

    // Called from CopyObject after payload commit. No locks; may overwrite oldest.
    void Record(BaseObject* from, BaseObject* to, size_t size) noexcept;

    // Best-effort scan (signal path). Matches exact from or interior of [from,from+size).
    // Returns true if any slot matches; fills *out with the matching entry.
    bool Lookup(uintptr_t addr, Entry* out) const noexcept;

    uint64_t TotalRecorded() const noexcept { return total_.load(std::memory_order_relaxed); }

private:
    TimeWindowEvacRing() = default;
    ~TimeWindowEvacRing() = default;
    TimeWindowEvacRing(const TimeWindowEvacRing&) = delete;
    TimeWindowEvacRing& operator=(const TimeWindowEvacRing&) = delete;

    std::atomic<uint64_t> seq_{ 0 };
    std::atomic<uint64_t> total_{ 0 };
    Entry slots_[CAP];
};

// AS-safe dump for signal path: look up si_addr (and its low-48) in the ring.
void PrintTimeWindowEvacLookup(const void* siAddr) noexcept;
} // namespace MapleRuntime

#endif // MRT_TIME_WINDOW_EVAC_RING_H
