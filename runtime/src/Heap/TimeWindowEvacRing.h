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
// Storage is mmap'd on first Record (not BSS) so the SO stays lean.
//
// Two layers:
//   (1) open-addressing hash of exact from-base (HASH_CAP=8M).
//   (2) ring of (from,to,size,preState) for detail (RING_CAP=1M).
class TimeWindowEvacRing {
public:
    static constexpr size_t RING_CAP = 1u << 20;  // 1M detail slots
    static constexpr size_t HASH_CAP = 1u << 23;  // 8M exact-from slots

    struct Entry {
        uintptr_t from = 0;
        uintptr_t to = 0;
        uint32_t size = 0;
        uint32_t preState = 0;
    };

    static TimeWindowEvacRing& Instance() noexcept;

    void Record(BaseObject* from, BaseObject* to, size_t size) noexcept;
    bool Lookup(uintptr_t addr, Entry* out) const noexcept;

    uint64_t TotalRecorded() const noexcept { return total_.load(std::memory_order_relaxed); }
    uint64_t HashInserts() const noexcept { return hashInserts_.load(std::memory_order_relaxed); }
    uint64_t HashFullDrops() const noexcept { return hashFullDrops_.load(std::memory_order_relaxed); }

private:
    TimeWindowEvacRing() = default;
    ~TimeWindowEvacRing() = default;
    TimeWindowEvacRing(const TimeWindowEvacRing&) = delete;
    TimeWindowEvacRing& operator=(const TimeWindowEvacRing&) = delete;

    void EnsureStorage() noexcept;

    std::atomic<uint64_t> seq_{ 0 };
    std::atomic<uint64_t> total_{ 0 };
    std::atomic<uint64_t> hashInserts_{ 0 };
    std::atomic<uint64_t> hashFullDrops_{ 0 };
    std::atomic<int> ready_{ 0 };
    Entry* slots_ = nullptr;                    // RING_CAP
    std::atomic<uintptr_t>* fromHash_ = nullptr; // HASH_CAP
};

void PrintTimeWindowEvacLookup(const void* siAddr) noexcept;
} // namespace MapleRuntime

#endif // MRT_TIME_WINDOW_EVAC_RING_H
