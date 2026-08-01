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
// Observation only. 1M slots (proven SEGV shape; larger durable sets
// shifted timing into unrelated CHECK abort paths).
class TimeWindowEvacRing {
public:
    static constexpr size_t CAP = 1u << 20; // 1M

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

private:
    TimeWindowEvacRing() = default;
    ~TimeWindowEvacRing() = default;
    TimeWindowEvacRing(const TimeWindowEvacRing&) = delete;
    TimeWindowEvacRing& operator=(const TimeWindowEvacRing&) = delete;

    std::atomic<uint64_t> seq_{ 0 };
    std::atomic<uint64_t> total_{ 0 };
    Entry slots_[CAP];
};

void PrintTimeWindowEvacLookup(const void* siAddr) noexcept;
} // namespace MapleRuntime

#endif // MRT_TIME_WINDOW_EVAC_RING_H
