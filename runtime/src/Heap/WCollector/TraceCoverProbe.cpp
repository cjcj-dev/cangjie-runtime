// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TraceCoverProbe.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <new>

#include "Base/LogFile.h"
#include "ObjectModel/BaseObject.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace TraceCoverProbe {
namespace {

// Open-address concurrent set of slot addresses (heap fields + roots).
// CAP = 16M entries × 8B ≈ 128MB peak while probe is on (diagnostic only).
constexpr size_t kCap = 1u << 24;
constexpr size_t kMask = kCap - 1;

std::atomic<uintptr_t>* gSlots = nullptr;
std::atomic<bool> gInited{ false };
std::atomic<uint64_t> gMarkAttempts{ 0 };
std::atomic<uint64_t> gMarkInserts{ 0 };
std::atomic<uint64_t> gMarkFull{ 0 };
std::atomic<uint64_t> gMigrateObjs{ 0 };
std::atomic<uint64_t> gMigrateBits{ 0 };

std::atomic<uint64_t> gHeapTraced{ 0 };
std::atomic<uint64_t> gHeapUntraced{ 0 };
std::atomic<uint64_t> gRootTraced{ 0 };
std::atomic<uint64_t> gRootUntraced{ 0 };
std::atomic<uint64_t> gPostflipFixedHeap{ 0 };
std::atomic<uint64_t> gPostflipFixedRoot{ 0 };

bool EnvOn()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_TRACE_COVER");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

size_t Hash(uintptr_t k)
{
    // SplitMix64-ish for pointer keys.
    uint64_t x = static_cast<uint64_t>(k);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<size_t>(x);
}

void EnsureInit()
{
    if (gInited.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!gInited.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        while (gSlots == nullptr) {
        }
        return;
    }
    auto* mem = new (std::nothrow) std::atomic<uintptr_t>[kCap];
    if (mem == nullptr) {
        VLOG(REPORT, "[GCV2][trace-cover] FATAL alloc set CAP=%zu failed", kCap);
        gInited.store(false, std::memory_order_release);
        return;
    }
    for (size_t i = 0; i < kCap; ++i) {
        mem[i].store(0, std::memory_order_relaxed);
    }
    gSlots = mem;
}

void ClearSet()
{
    if (gSlots == nullptr) {
        return;
    }
    for (size_t i = 0; i < kCap; ++i) {
        gSlots[i].store(0, std::memory_order_relaxed);
    }
}

void Insert(uintptr_t key)
{
    if (key == 0 || gSlots == nullptr) {
        return;
    }
    gMarkAttempts.fetch_add(1, std::memory_order_relaxed);
    size_t h = Hash(key) & kMask;
    for (size_t n = 0; n < kCap; ++n) {
        size_t idx = (h + n) & kMask;
        uintptr_t cur = gSlots[idx].load(std::memory_order_relaxed);
        if (cur == key) {
            return;
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (gSlots[idx].compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
                gMarkInserts.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expected == key) {
                return;
            }
        }
    }
    gMarkFull.fetch_add(1, std::memory_order_relaxed);
}

bool Contains(uintptr_t key)
{
    if (key == 0 || gSlots == nullptr) {
        return false;
    }
    size_t h = Hash(key) & kMask;
    for (size_t n = 0; n < kCap; ++n) {
        size_t idx = (h + n) & kMask;
        uintptr_t cur = gSlots[idx].load(std::memory_order_relaxed);
        if (cur == 0) {
            return false;
        }
        if (cur == key) {
            return true;
        }
    }
    return false;
}

} // namespace

bool Enabled()
{
    return EnvOn();
}

void BeginMajorCycle()
{
    if (!EnvOn()) {
        return;
    }
    EnsureInit();
    ClearSet();
    gMarkAttempts.store(0, std::memory_order_relaxed);
    gMarkInserts.store(0, std::memory_order_relaxed);
    gMarkFull.store(0, std::memory_order_relaxed);
    gMigrateObjs.store(0, std::memory_order_relaxed);
    gMigrateBits.store(0, std::memory_order_relaxed);
    gHeapTraced.store(0, std::memory_order_relaxed);
    gHeapUntraced.store(0, std::memory_order_relaxed);
    gRootTraced.store(0, std::memory_order_relaxed);
    gRootUntraced.store(0, std::memory_order_relaxed);
    gPostflipFixedHeap.store(0, std::memory_order_relaxed);
    gPostflipFixedRoot.store(0, std::memory_order_relaxed);
    VLOG(REPORT, "[GCV2][trace-cover] begin major cycle env=MRT_GCV2_TRACE_COVER=1");
}

void MarkSlot(const void* slotAddr)
{
    if (!EnvOn() || slotAddr == nullptr) {
        return;
    }
    EnsureInit();
    Insert(reinterpret_cast<uintptr_t>(slotAddr));
}

void MigrateObject(const BaseObject* fromObj, const BaseObject* toObj, size_t size)
{
    if (!EnvOn() || fromObj == nullptr || toObj == nullptr || fromObj == toObj || size == 0) {
        return;
    }
    if (gSlots == nullptr) {
        return;
    }
    // Walk 8-byte-aligned addresses in the copied span; migrate set membership.
    // RefField slots are pointer-aligned; scanning every 8B covers all slots.
    const uintptr_t fromBase = reinterpret_cast<uintptr_t>(fromObj);
    const uintptr_t toBase = reinterpret_cast<uintptr_t>(toObj);
    size_t moved = 0;
    for (size_t off = 0; off + sizeof(void*) <= size; off += sizeof(void*)) {
        uintptr_t fromSlot = fromBase + off;
        if (!Contains(fromSlot)) {
            continue;
        }
        Insert(toBase + off);
        ++moved;
    }
    if (moved != 0) {
        gMigrateObjs.fetch_add(1, std::memory_order_relaxed);
        gMigrateBits.fetch_add(moved, std::memory_order_relaxed);
    }
}

void AccountFixed(const void* slotAddr, int kind)
{
    if (!EnvOn() || slotAddr == nullptr) {
        return;
    }
    bool traced = Contains(reinterpret_cast<uintptr_t>(slotAddr));
    if (kind == 0) {
        if (traced) {
            gHeapTraced.fetch_add(1, std::memory_order_relaxed);
        } else {
            gHeapUntraced.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (traced) {
            gRootTraced.fetch_add(1, std::memory_order_relaxed);
        } else {
            gRootUntraced.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NotePostflipFixedBaseline(size_t fixedHeap, size_t fixedRoot)
{
    if (!EnvOn()) {
        return;
    }
    gPostflipFixedHeap.store(fixedHeap, std::memory_order_relaxed);
    gPostflipFixedRoot.store(fixedRoot, std::memory_order_relaxed);
}

void ReportPostflip()
{
    if (!EnvOn()) {
        return;
    }
    const uint64_t ht = gHeapTraced.load(std::memory_order_relaxed);
    const uint64_t hu = gHeapUntraced.load(std::memory_order_relaxed);
    const uint64_t rt = gRootTraced.load(std::memory_order_relaxed);
    const uint64_t ru = gRootUntraced.load(std::memory_order_relaxed);
    const uint64_t total = ht + hu + rt + ru;
    const uint64_t untraced = hu + ru;
    double coverPct = total == 0 ? 100.0 : (100.0 * static_cast<double>(ht + rt) / static_cast<double>(total));
    VLOG(REPORT,
         "[GCV2][trace-cover] postflip fixed_total=%llu heap_traced=%llu heap_untraced=%llu "
         "root_traced=%llu root_untraced=%llu cover_pct=%.2f "
         "mark_attempts=%llu mark_inserts=%llu mark_full=%llu migrate_objs=%llu migrate_bits=%llu "
         "account_fixed_heap=%llu account_fixed_root=%llu env=MRT_GCV2_TRACE_COVER=1",
         static_cast<unsigned long long>(total), static_cast<unsigned long long>(ht),
         static_cast<unsigned long long>(hu), static_cast<unsigned long long>(rt),
         static_cast<unsigned long long>(ru), coverPct, static_cast<unsigned long long>(gMarkAttempts.load()),
         static_cast<unsigned long long>(gMarkInserts.load()), static_cast<unsigned long long>(gMarkFull.load()),
         static_cast<unsigned long long>(gMigrateObjs.load()), static_cast<unsigned long long>(gMigrateBits.load()),
         static_cast<unsigned long long>(gPostflipFixedHeap.load()),
         static_cast<unsigned long long>(gPostflipFixedRoot.load()));
}

} // namespace TraceCoverProbe
} // namespace MapleRuntime
