// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/NoTracedDiag.h"

#include <atomic>
#include <cstdint>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/Verify/MarkCompleteVerify.h"

namespace MapleRuntime {
namespace NoTracedDiag {
namespace {

// Direct-mapped, fixed size, no allocation: NoteTrace runs once per traced object.
constexpr size_t kSlots = 256;
constexpr size_t kSlotMask = kSlots - 1;

struct Slot {
    std::atomic<uintptr_t> addr{ 0 };
    std::atomic<uint64_t> traced{ 0 };
    std::atomic<uint64_t> moved{ 0 };
};

Slot g_slots[kSlots];
std::atomic<uint64_t> g_watched{ 0 };
std::atomic<uint64_t> g_collisions{ 0 };
std::atomic<uint64_t> g_crashJoinHits{ 0 };

// Object addresses are at least 8-byte aligned, so the low bits carry no entropy.
size_t SlotOf(uintptr_t addr) { return static_cast<size_t>((addr >> 4) & kSlotMask); }

} // namespace

bool Enabled() { return MarkCompleteVerify::Enabled(); }

void Watch(const BaseObject* obj)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    Slot& slot = g_slots[SlotOf(addr)];
    uintptr_t expected = slot.addr.load(std::memory_order_acquire);
    if (expected == addr) {
        return; // already watched
    }
    if (expected != 0) {
        // Another holder owns this slot. Say so rather than evicting: an evicted
        // watch would report traced=0 for a holder nobody was watching.
        g_collisions.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (slot.addr.compare_exchange_strong(expected, addr, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        slot.traced.store(0, std::memory_order_relaxed);
        slot.moved.store(0, std::memory_order_relaxed);
        g_watched.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteTrace(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    Slot& slot = g_slots[SlotOf(addr)];
    if (slot.addr.load(std::memory_order_acquire) == addr) {
        slot.traced.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done)
{
    (void)size;
    (void)done;
    if (!Enabled() || fromAddr == nullptr || toAddr == nullptr) {
        return;
    }
    // A watched holder that moves would otherwise stop matching and read as
    // "never traced again", which is exactly the wrong conclusion.
    const uintptr_t from = reinterpret_cast<uintptr_t>(fromAddr);
    Slot& slot = g_slots[SlotOf(from)];
    if (slot.addr.load(std::memory_order_acquire) != from) {
        return;
    }
    slot.moved.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t to = reinterpret_cast<uintptr_t>(toAddr);
    Slot& dst = g_slots[SlotOf(to)];
    uintptr_t empty = 0;
    if (dst.addr.compare_exchange_strong(empty, to, std::memory_order_acq_rel, std::memory_order_acquire)) {
        dst.traced.store(0, std::memory_order_relaxed);
        dst.moved.store(0, std::memory_order_relaxed);
    } else {
        g_collisions.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteCrashJoin(uintptr_t holderCrash, uintptr_t holderCas)
{
    if (!Enabled()) {
        return;
    }
    for (uintptr_t candidate : { holderCrash, holderCas }) {
        if (candidate == 0) {
            continue;
        }
        Slot& slot = g_slots[SlotOf(candidate)];
        if (slot.addr.load(std::memory_order_acquire) != candidate) {
            continue;
        }
        g_crashJoinHits.fetch_add(1, std::memory_order_relaxed);
        LOG(RTLOG_ERROR, "[GCV2][notraced] CRASH_JOIN holder=%#zx traced=%llu moved=%llu",
            static_cast<size_t>(candidate),
            static_cast<unsigned long long>(slot.traced.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.moved.load(std::memory_order_relaxed)));
    }
}

void Report(const char* point)
{
    if (!Enabled()) {
        return;
    }
    const uint64_t watched = g_watched.load(std::memory_order_relaxed);
    // Emit the header even at zero: a silent probe is indistinguishable from a
    // hollowed one, which is the state this file was in.
    LOG(RTLOG_ERROR, "[GCV2][notraced] point=%s watched=%llu collisions=%llu crashJoinHits=%llu",
        point == nullptr ? "?" : point, static_cast<unsigned long long>(watched),
        static_cast<unsigned long long>(g_collisions.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_crashJoinHits.load(std::memory_order_relaxed)));
    if (watched == 0) {
        return;
    }
    for (size_t i = 0; i < kSlots; ++i) {
        const uintptr_t addr = g_slots[i].addr.load(std::memory_order_acquire);
        if (addr == 0) {
            continue;
        }
        const uint64_t traced = g_slots[i].traced.load(std::memory_order_relaxed);
        // traced == 0 is case 1: the holder's live bit was painted without its
        // fields ever being scanned. traced > 0 is case 2 or 3.
        LOG(RTLOG_ERROR, "[GCV2][notraced] holder=%#zx traced=%llu moved=%llu case=%s", static_cast<size_t>(addr),
            static_cast<unsigned long long>(traced),
            static_cast<unsigned long long>(g_slots[i].moved.load(std::memory_order_relaxed)),
            traced == 0 ? "painted-not-followed" : "followed");
    }
}

} // namespace NoTracedDiag
} // namespace MapleRuntime
