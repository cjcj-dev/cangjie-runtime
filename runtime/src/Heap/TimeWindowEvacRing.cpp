// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TimeWindowEvacRing.h"

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Base/SysCall.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "securec.h"

namespace MapleRuntime {
namespace {
inline size_t MixPtr(uintptr_t x) noexcept
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return static_cast<size_t>(x);
}
} // namespace

TimeWindowEvacRing& TimeWindowEvacRing::Instance() noexcept
{
    static TimeWindowEvacRing instance;
    return instance;
}

void TimeWindowEvacRing::EnsureStorage() noexcept
{
    if (ready_.load(std::memory_order_acquire) != 0) {
        return;
    }
    static std::atomic<int> mapping{ 0 };
    int expected = 0;
    if (mapping.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        const size_t ringBytes = RING_CAP * sizeof(Entry);
        const size_t hashBytes = HASH_CAP * sizeof(std::atomic<uintptr_t>);
        void* ring = mmap(nullptr, ringBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void* hash = mmap(nullptr, hashBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ring == MAP_FAILED || hash == MAP_FAILED) {
            if (ring != MAP_FAILED) {
                munmap(ring, ringBytes);
            }
            if (hash != MAP_FAILED) {
                munmap(hash, hashBytes);
            }
            mapping.store(0, std::memory_order_release);
            return;
        }
        slots_ = static_cast<Entry*>(ring);
        fromHash_ = static_cast<std::atomic<uintptr_t>*>(hash);
        ready_.store(1, std::memory_order_release);
    } else {
        while (ready_.load(std::memory_order_acquire) == 0) {
            sched_yield();
        }
    }
}

void TimeWindowEvacRing::Record(BaseObject* from, BaseObject* to, size_t size) noexcept
{
    if (from == nullptr || to == nullptr || from == to || size == 0) {
        return;
    }
    EnsureStorage();
    if (ready_.load(std::memory_order_acquire) == 0) {
        return;
    }
    uint32_t preState = static_cast<uint32_t>(from->GetObjectState().GetStateCode());
    const uintptr_t fromU = reinterpret_cast<uintptr_t>(from);
    const uintptr_t toU = reinterpret_cast<uintptr_t>(to);

    size_t i = MixPtr(fromU) & (HASH_CAP - 1);
    bool inserted = false;
    for (size_t probe = 0; probe < 64; ++probe) {
        uintptr_t expected = 0;
        if (fromHash_[i].compare_exchange_strong(expected, fromU, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
            hashInserts_.fetch_add(1, std::memory_order_relaxed);
            inserted = true;
            break;
        }
        if (expected == fromU) {
            inserted = true;
            break;
        }
        i = (i + 1) & (HASH_CAP - 1);
    }
    if (!inserted) {
        hashFullDrops_.fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t n = seq_.fetch_add(1, std::memory_order_relaxed);
    Entry& slot = slots_[n & (RING_CAP - 1)];
    slot.from = fromU;
    slot.to = toU;
    slot.size = static_cast<uint32_t>(size > 0xffffffffu ? 0xffffffffu : size);
    slot.preState = preState;
    std::atomic_thread_fence(std::memory_order_release);
    total_.fetch_add(1, std::memory_order_relaxed);
}

bool TimeWindowEvacRing::Lookup(uintptr_t addr, Entry* out) const noexcept
{
    if (addr == 0 || out == nullptr || ready_.load(std::memory_order_acquire) == 0) {
        return false;
    }
    // detail ring first (preState)
    const uint64_t total = total_.load(std::memory_order_acquire);
    if (total > 0) {
        const size_t n = total < RING_CAP ? static_cast<size_t>(total) : RING_CAP;
        const uint64_t seq = seq_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t idx = (seq - 1 - i) & (RING_CAP - 1);
            const Entry& e = slots_[idx];
            if (e.from == 0 || e.size == 0) {
                continue;
            }
            if (addr >= e.from && addr - e.from < e.size) {
                *out = e;
                return true;
            }
        }
    }
    // exact-from durable hash
    size_t i = MixPtr(addr) & (HASH_CAP - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
        uintptr_t v = fromHash_[i].load(std::memory_order_relaxed);
        if (v == 0) {
            break;
        }
        if (v == addr) {
            out->from = addr;
            out->to = 0;
            out->size = 0;
            out->preState = 0xffffffffu;
            return true;
        }
        i = (i + 1) & (HASH_CAP - 1);
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

    char buf[640];
    int n = sprintf_s(buf, sizeof(buf),
                      "%d E [TimeWindow] si=%p hi16=%u low48=%p total=%llu hashIns=%llu hashDrop=%llu "
                      "found_raw=%d from=%p to=%p size=%u preState=%u "
                      "found_low48=%d from48=%p to48=%p size48=%u preState48=%u\n",
                      static_cast<int>(GetTid()), siAddr, high16, reinterpret_cast<void*>(low48),
                      static_cast<unsigned long long>(ring.TotalRecorded()),
                      static_cast<unsigned long long>(ring.HashInserts()),
                      static_cast<unsigned long long>(ring.HashFullDrops()), foundRaw,
                      reinterpret_cast<void*>(hit.from), reinterpret_cast<void*>(hit.to), hit.size, hit.preState,
                      found48, reinterpret_cast<void*>(hit48.from), reinterpret_cast<void*>(hit48.to), hit48.size,
                      hit48.preState);
    if (n > 0) {
        (void)write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}
} // namespace MapleRuntime
