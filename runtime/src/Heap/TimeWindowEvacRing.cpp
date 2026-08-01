// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TimeWindowEvacRing.h"

#include <unistd.h>

#include "Base/SysCall.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "securec.h"

namespace MapleRuntime {
TimeWindowEvacRing& TimeWindowEvacRing::Instance() noexcept
{
    static TimeWindowEvacRing instance;
    return instance;
}

void TimeWindowEvacRing::Record(BaseObject* from, BaseObject* to, size_t size) noexcept
{
    if (from == nullptr || to == nullptr || from == to || size == 0) {
        return;
    }
    uint32_t preState = static_cast<uint32_t>(from->GetObjectState().GetStateCode());
    const uint64_t n = seq_.fetch_add(1, std::memory_order_relaxed);
    Entry& slot = slots_[n & (CAP - 1)];
    slot.from = reinterpret_cast<uintptr_t>(from);
    slot.to = reinterpret_cast<uintptr_t>(to);
    slot.size = static_cast<uint32_t>(size > 0xffffffffu ? 0xffffffffu : size);
    slot.preState = preState;
    std::atomic_thread_fence(std::memory_order_release);
    total_.fetch_add(1, std::memory_order_relaxed);
}

bool TimeWindowEvacRing::Lookup(uintptr_t addr, Entry* out) const noexcept
{
    if (addr == 0 || out == nullptr) {
        return false;
    }
    const uint64_t total = total_.load(std::memory_order_acquire);
    if (total == 0) {
        return false;
    }
    // Cap scan to last 64K newest entries in signal path (AS-safe budget).
    // Positive hit in recent window is decisive; full 1M scan is too heavy here.
    constexpr size_t kSigScan = 1u << 16;
    const size_t n = total < CAP ? static_cast<size_t>(total) : CAP;
    const size_t scan = n < kSigScan ? n : kSigScan;
    const uint64_t seq = seq_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < scan; ++i) {
        const uint64_t idx = (seq - 1 - i) & (CAP - 1);
        const Entry& e = slots_[idx];
        if (e.from == 0 || e.size == 0) {
            continue;
        }
        if (addr >= e.from && addr - e.from < e.size) {
            *out = e;
            return true;
        }
    }
    return false;
}

void PrintTimeWindowEvacLookup(const void* siAddr) noexcept
{
    const uintptr_t raw = reinterpret_cast<uintptr_t>(siAddr);
    const uintptr_t low48 = raw & ((1ull << 48) - 1);
    const unsigned high16 = static_cast<unsigned>((raw >> 48) & 0xffffu);

    TimeWindowEvacRing& ring = TimeWindowEvacRing::Instance();
    TimeWindowEvacRing::Entry hit {};
    int foundRaw = ring.Lookup(raw, &hit) ? 1 : 0;
    TimeWindowEvacRing::Entry hit48 {};
    int found48 = 0;
    if (low48 != raw) {
        found48 = ring.Lookup(low48, &hit48) ? 1 : 0;
    } else {
        found48 = foundRaw;
        hit48 = hit;
    }
    const uint64_t total = ring.TotalRecorded();
    const unsigned wrap = total > TimeWindowEvacRing::CAP ? 1u : 0u;

    char buf[512];
    int n = sprintf_s(buf, sizeof(buf),
                      "%d E [TimeWindow] si=%p hi16=%u low48=%p total=%llu wrap=%u "
                      "found_raw=%d from=%p to=%p size=%u preState=%u "
                      "found_low48=%d from48=%p to48=%p size48=%u preState48=%u\n",
                      static_cast<int>(GetTid()), siAddr, high16, reinterpret_cast<void*>(low48),
                      static_cast<unsigned long long>(total), wrap, foundRaw,
                      reinterpret_cast<void*>(hit.from), reinterpret_cast<void*>(hit.to), hit.size, hit.preState,
                      found48, reinterpret_cast<void*>(hit48.from), reinterpret_cast<void*>(hit48.to), hit48.size,
                      hit48.preState);
    if (n > 0) {
        (void)write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}
} // namespace MapleRuntime
