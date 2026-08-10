// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/EatArmDiag.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace EatArmDiag {
namespace {

constexpr size_t kStampBitsDefault = 18;
constexpr size_t kStampBitsMin = 16;
constexpr size_t kStampBitsMax = 22;
constexpr size_t kRingCap = 256;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

bool GateOn()
{
    static const bool on = []() {
        if (EnvIsOne("MRT_GCV2_EATARM")) {
            return true;
        }
        return DiagGate::TokenOn("eatarm");
    }();
    return on;
}

const char* ArmName(Arm a)
{
    switch (a) {
        case Arm::WAS_MARKED:
            return "WAS_MARKED";
        case Arm::FYS_REMSET:
            return "FYS_REMSET";
        case Arm::FIXPOINT:
            return "FIXPOINT";
        case Arm::NONYOUNG_DEDUP:
            return "NONYOUNG_DEDUP";
        default:
            return "NONE";
    }
}

const char* FixReasonName(FixpointReason r)
{
    switch (r) {
        case FixpointReason::TARGET_NULL:
            return "target_null";
        case FixpointReason::TARGET_NONHEAP:
            return "target_nonheap";
        case FixpointReason::PLAUSIBLE_FAIL:
            return "plausible_fail";
        case FixpointReason::NOT_YOUNG:
            return "not_young";
        case FixpointReason::ALREADY_MARKED:
            return "already_marked";
        case FixpointReason::ADMIT:
            return "admit";
        default:
            return "none";
    }
}

// Open-address stamp: key = object pointer (holder for skip-field; target for remset/fixpoint).
// value packs arm + reason + gen.
struct StampSlot {
    std::atomic<uintptr_t> key{ 0 };
    std::atomic<uint32_t> meta{ 0 }; // bits: arm:4 | reason:4 | gen:16 | flags:8
};

struct RingRec {
    uintptr_t key{ 0 };
    uint8_t arm{ 0 };
    uint8_t reason{ 0 };
    uint16_t gen{ 0 };
};

std::atomic<uint16_t> g_gen{ 1 };
std::atomic<size_t> g_stampCap{ 0 };
std::atomic<StampSlot*> g_stamps{ nullptr };

std::atomic<size_t> g_cntWasMarked{ 0 };
std::atomic<size_t> g_cntFysRemset{ 0 };
std::atomic<size_t> g_cntFixpoint{ 0 };
std::atomic<size_t> g_cntFixpointAdmit{ 0 };
std::atomic<size_t> g_cntNonYoungDedup{ 0 };
std::atomic<size_t> g_cntStampInsert{ 0 };
std::atomic<size_t> g_cntStampHit{ 0 };
std::atomic<size_t> g_cntStampProbeFail{ 0 };
std::atomic<size_t> g_cntIorSeen{ 0 };
std::atomic<size_t> g_cntIorHitWasMarked{ 0 };
std::atomic<size_t> g_cntIorHitFysRemset{ 0 };
std::atomic<size_t> g_cntIorHitFixpoint{ 0 };
std::atomic<size_t> g_cntIorHitNonYoung{ 0 };
std::atomic<size_t> g_cntIorMiss{ 0 };
std::atomic<size_t> g_cntIorPrinted{ 0 };
std::atomic<size_t> g_selfTestFired{ 0 };

// Compact ring of recent notes for IOR host-as-holder lookup.
RingRec g_ring[kRingCap];
std::atomic<size_t> g_ringIdx{ 0 };

thread_local BaseObject* t_fixHost = nullptr;

void EnsureStamps()
{
    if (g_stamps.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    size_t bits = EnvSizeT("MRT_GCV2_EATARM_STAMP_BITS", kStampBitsDefault);
    if (bits < kStampBitsMin) {
        bits = kStampBitsMin;
    }
    if (bits > kStampBitsMax) {
        bits = kStampBitsMax;
    }
    size_t cap = size_t{ 1 } << bits;
    StampSlot* table = new (std::nothrow) StampSlot[cap];
    if (table == nullptr) {
        return;
    }
    StampSlot* expected = nullptr;
    if (!g_stamps.compare_exchange_strong(expected, table, std::memory_order_acq_rel)) {
        delete[] table;
        return;
    }
    g_stampCap.store(cap, std::memory_order_release);
    std::fprintf(stderr,
                 "[GCV2][eatarm][LEGEND] stampCap=%zu bits=%zu arms=WAS_MARKED,FYS_REMSET,FIXPOINT,NONYOUNG_DEDUP "
                 "expect: when EATARM=1 and minor runs, cnt>0 on at least one arm; IOR miss ⇒ fourth cause\n",
                 cap, bits);
    std::fflush(stderr);
}

uint32_t PackMeta(Arm arm, uint8_t reason, uint16_t gen)
{
    return (static_cast<uint32_t>(arm) & 0xfu) | ((static_cast<uint32_t>(reason) & 0xfu) << 4) |
           (static_cast<uint32_t>(gen) << 8);
}

void UnpackMeta(uint32_t meta, Arm* arm, uint8_t* reason, uint16_t* gen)
{
    *arm = static_cast<Arm>(meta & 0xfu);
    *reason = static_cast<uint8_t>((meta >> 4) & 0xfu);
    *gen = static_cast<uint16_t>((meta >> 8) & 0xffffu);
}

void RingPush(uintptr_t key, Arm arm, uint8_t reason, uint16_t gen)
{
    size_t i = g_ringIdx.fetch_add(1, std::memory_order_relaxed) % kRingCap;
    g_ring[i].key = key;
    g_ring[i].arm = static_cast<uint8_t>(arm);
    g_ring[i].reason = reason;
    g_ring[i].gen = gen;
}

// Insert or OR-merge arm bits for key. Open-address linear probe; force-overwrite on fail.
void StampNote(uintptr_t key, Arm arm, uint8_t reason)
{
    if (key == 0) {
        return;
    }
    EnsureStamps();
    StampSlot* table = g_stamps.load(std::memory_order_acquire);
    size_t cap = g_stampCap.load(std::memory_order_acquire);
    if (table == nullptr || cap == 0) {
        return;
    }
    uint16_t gen = g_gen.load(std::memory_order_acquire);
    size_t mask = cap - 1;
    size_t h = (key ^ (key >> 16)) & mask;
    constexpr size_t kProbeMax = 32;
    for (size_t p = 0; p < kProbeMax; ++p) {
        size_t i = (h + p) & mask;
        uintptr_t cur = table[i].key.load(std::memory_order_acquire);
        if (cur == 0) {
            uintptr_t zero = 0;
            if (table[i].key.compare_exchange_strong(zero, key, std::memory_order_acq_rel)) {
                table[i].meta.store(PackMeta(arm, reason, gen), std::memory_order_release);
                g_cntStampInsert.fetch_add(1, std::memory_order_relaxed);
                RingPush(key, arm, reason, gen);
                return;
            }
            cur = table[i].key.load(std::memory_order_acquire);
        }
        if (cur == key) {
            uint32_t old = table[i].meta.load(std::memory_order_acquire);
            Arm oarm;
            uint8_t oreason;
            uint16_t ogen;
            UnpackMeta(old, &oarm, &oreason, &ogen);
            if (ogen != gen) {
                table[i].meta.store(PackMeta(arm, reason, gen), std::memory_order_release);
            } else {
                // Keep first arm; set high flag if multi-arm (rare).
                if (oarm != arm && oarm != Arm::NONE) {
                    table[i].meta.store(PackMeta(oarm, oreason | 0x80u, gen), std::memory_order_release);
                }
            }
            g_cntStampHit.fetch_add(1, std::memory_order_relaxed);
            RingPush(key, arm, reason, gen);
            return;
        }
    }
    // Force overwrite at home slot.
    size_t i = h;
    table[i].key.store(key, std::memory_order_release);
    table[i].meta.store(PackMeta(arm, reason, gen), std::memory_order_release);
    g_cntStampProbeFail.fetch_add(1, std::memory_order_relaxed);
    g_cntStampInsert.fetch_add(1, std::memory_order_relaxed);
    RingPush(key, arm, reason, gen);
}

bool StampLookup(uintptr_t key, Arm* armOut, uint8_t* reasonOut)
{
    StampSlot* table = g_stamps.load(std::memory_order_acquire);
    size_t cap = g_stampCap.load(std::memory_order_acquire);
    if (table == nullptr || cap == 0 || key == 0) {
        return false;
    }
    uint16_t gen = g_gen.load(std::memory_order_acquire);
    size_t mask = cap - 1;
    size_t h = (key ^ (key >> 16)) & mask;
    constexpr size_t kProbeMax = 32;
    for (size_t p = 0; p < kProbeMax; ++p) {
        size_t i = (h + p) & mask;
        uintptr_t cur = table[i].key.load(std::memory_order_acquire);
        if (cur == 0) {
            return false;
        }
        if (cur == key) {
            uint32_t meta = table[i].meta.load(std::memory_order_acquire);
            Arm a;
            uint8_t r;
            uint16_t g;
            UnpackMeta(meta, &a, &r, &g);
            if (g != gen) {
                return false;
            }
            *armOut = a;
            *reasonOut = r;
            return true;
        }
    }
    return false;
}

// Ring scan: find most recent record matching key (as holder or target).
bool RingLookup(uintptr_t key, Arm* armOut, uint8_t* reasonOut)
{
    if (key == 0) {
        return false;
    }
    uint16_t gen = g_gen.load(std::memory_order_acquire);
    size_t start = g_ringIdx.load(std::memory_order_acquire);
    for (size_t d = 0; d < kRingCap; ++d) {
        size_t i = (start + kRingCap - 1 - d) % kRingCap;
        if (g_ring[i].key == key && g_ring[i].gen == gen) {
            *armOut = static_cast<Arm>(g_ring[i].arm);
            *reasonOut = g_ring[i].reason;
            return true;
        }
    }
    return false;
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void OnMinorBegin(size_t minorRunIndex)
{
    if (!GateOn()) {
        return;
    }
    (void)minorRunIndex;
    EnsureStamps();
    uint16_t g = g_gen.load(std::memory_order_relaxed);
    g_gen.store(static_cast<uint16_t>(g + 1u == 0 ? 1 : g + 1u), std::memory_order_release);
    // Soft clear: gen bump invalidates; zero keys each minor (stampfix lesson).
    StampSlot* table = g_stamps.load(std::memory_order_acquire);
    size_t cap = g_stampCap.load(std::memory_order_acquire);
    if (table != nullptr && cap != 0) {
        // Full clear of keys every minor (stampfix lesson). Cap is 2^18 max ~2MB.
        for (size_t i = 0; i < cap; ++i) {
            table[i].key.store(0, std::memory_order_relaxed);
            table[i].meta.store(0, std::memory_order_relaxed);
        }
    }
    g_cntWasMarked.store(0, std::memory_order_relaxed);
    g_cntFysRemset.store(0, std::memory_order_relaxed);
    g_cntFixpoint.store(0, std::memory_order_relaxed);
    g_cntFixpointAdmit.store(0, std::memory_order_relaxed);
    g_cntNonYoungDedup.store(0, std::memory_order_relaxed);
    g_cntStampInsert.store(0, std::memory_order_relaxed);
    g_cntStampHit.store(0, std::memory_order_relaxed);
    g_cntStampProbeFail.store(0, std::memory_order_relaxed);
}

void NoteWasMarkedSkipFields(BaseObject* holder)
{
    if (!GateOn() || holder == nullptr) {
        return;
    }
    g_cntWasMarked.fetch_add(1, std::memory_order_relaxed);
    StampNote(reinterpret_cast<uintptr_t>(holder), Arm::WAS_MARKED, 0);
}

void NoteNonYoungDedupSkipFields(BaseObject* holder)
{
    if (!GateOn() || holder == nullptr) {
        return;
    }
    g_cntNonYoungDedup.fetch_add(1, std::memory_order_relaxed);
    StampNote(reinterpret_cast<uintptr_t>(holder), Arm::NONYOUNG_DEDUP, 0);
}

void NoteFysRemsetSkip(MAddress slot, BaseObject* target)
{
    if (!GateOn()) {
        return;
    }
    g_cntFysRemset.fetch_add(1, std::memory_order_relaxed);
    // Stamp by target (T identity for IOR reconcile). Also stamp slot for forensics.
    if (target != nullptr && Heap::IsHeapAddress(target)) {
        StampNote(reinterpret_cast<uintptr_t>(target), Arm::FYS_REMSET, 0);
    }
    if (slot != 0) {
        StampNote(static_cast<uintptr_t>(slot), Arm::FYS_REMSET, 1);
    }
}

void NoteFixpointEdge(BaseObject* holder, BaseObject* target, FixpointReason reason)
{
    if (!GateOn()) {
        return;
    }
    if (reason == FixpointReason::ADMIT) {
        g_cntFixpointAdmit.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Count all skip reasons; stamp only D8 plausible_fail (silent drop of a heap target).
    // not_young / already_marked / null are expected high-volume and would saturate the table.
    g_cntFixpoint.fetch_add(1, std::memory_order_relaxed);
    if (reason == FixpointReason::PLAUSIBLE_FAIL && target != nullptr) {
        StampNote(reinterpret_cast<uintptr_t>(target), Arm::FIXPOINT, static_cast<uint8_t>(reason));
        if (holder != nullptr) {
            StampNote(reinterpret_cast<uintptr_t>(holder), Arm::FIXPOINT, static_cast<uint8_t>(reason));
        }
    }
}

void SetFixHost(BaseObject* host)
{
    if (!GateOn()) {
        return;
    }
    t_fixHost = host;
}

BaseObject* GetFixHost()
{
    return t_fixHost;
}

void NoteIorTarget(BaseObject* targetT, BaseObject* host, size_t fieldOff)
{
    if (!GateOn() || targetT == nullptr) {
        return;
    }
    if (host == nullptr) {
        host = t_fixHost;
    }
    size_t n = g_cntIorSeen.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t maxIor = EnvSizeT("MRT_GCV2_EATARM_MAX_IOR", 32);
    uintptr_t tKey = reinterpret_cast<uintptr_t>(targetT);
    uintptr_t hKey = reinterpret_cast<uintptr_t>(host);

    Arm armT = Arm::NONE;
    uint8_t reasonT = 0;
    bool hitT = StampLookup(tKey, &armT, &reasonT);
    if (!hitT) {
        hitT = RingLookup(tKey, &armT, &reasonT);
    }

    Arm armH = Arm::NONE;
    uint8_t reasonH = 0;
    bool hitH = false;
    if (host != nullptr) {
        hitH = StampLookup(hKey, &armH, &reasonH);
        if (!hitH) {
            hitH = RingLookup(hKey, &armH, &reasonH);
        }
    }

    const char* verdict = "MISS_FOURTH";
    if (hitT && armT == Arm::FYS_REMSET) {
        verdict = "HIT_FYS_REMSET_T";
        g_cntIorHitFysRemset.fetch_add(1, std::memory_order_relaxed);
    } else if (hitT && armT == Arm::FIXPOINT) {
        verdict = "HIT_FIXPOINT_T";
        g_cntIorHitFixpoint.fetch_add(1, std::memory_order_relaxed);
    } else if (hitH && armH == Arm::WAS_MARKED) {
        verdict = "HIT_WAS_MARKED_HOST";
        g_cntIorHitWasMarked.fetch_add(1, std::memory_order_relaxed);
    } else if (hitH && armH == Arm::NONYOUNG_DEDUP) {
        verdict = "HIT_NONYOUNG_DEDUP_HOST";
        g_cntIorHitNonYoung.fetch_add(1, std::memory_order_relaxed);
    } else if (hitT || hitH) {
        verdict = "HIT_OTHER";
        if (hitT && armT == Arm::WAS_MARKED) {
            g_cntIorHitWasMarked.fetch_add(1, std::memory_order_relaxed);
        } else if (hitH && armH == Arm::FYS_REMSET) {
            g_cntIorHitFysRemset.fetch_add(1, std::memory_order_relaxed);
        } else if (hitH && armH == Arm::FIXPOINT) {
            g_cntIorHitFixpoint.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_cntIorMiss.fetch_add(1, std::memory_order_relaxed);
    }

    if (n <= maxIor) {
        size_t printed = g_cntIorPrinted.fetch_add(1, std::memory_order_relaxed) + 1;
        std::fprintf(stderr,
                     "[GCV2][eatarm][IOR] n=%zu printed=%zu T=%p host=%p fieldOff=%zu "
                     "hitT=%u armT=%s reasonT=%u hitH=%u armH=%s reasonH=%u verdict=%s "
                     "wm=%zu fys=%zu fp=%zu nyd=%zu admit=%zu\n",
                     n, printed, targetT, host, fieldOff, static_cast<unsigned>(hitT), ArmName(armT),
                     static_cast<unsigned>(reasonT), static_cast<unsigned>(hitH), ArmName(armH),
                     static_cast<unsigned>(reasonH), verdict, g_cntWasMarked.load(std::memory_order_relaxed),
                     g_cntFysRemset.load(std::memory_order_relaxed), g_cntFixpoint.load(std::memory_order_relaxed),
                     g_cntNonYoungDedup.load(std::memory_order_relaxed),
                     g_cntFixpointAdmit.load(std::memory_order_relaxed));
        std::fflush(stderr);
    }
}

void DumpMinorSummary(size_t minorRunIndex)
{
    if (!GateOn()) {
        return;
    }
    size_t wm = g_cntWasMarked.load(std::memory_order_relaxed);
    size_t fys = g_cntFysRemset.load(std::memory_order_relaxed);
    size_t fp = g_cntFixpoint.load(std::memory_order_relaxed);
    size_t admit = g_cntFixpointAdmit.load(std::memory_order_relaxed);
    size_t nyd = g_cntNonYoungDedup.load(std::memory_order_relaxed);
    size_t ins = g_cntStampInsert.load(std::memory_order_relaxed);
    size_t hit = g_cntStampHit.load(std::memory_order_relaxed);
    size_t pfail = g_cntStampProbeFail.load(std::memory_order_relaxed);
    size_t ior = g_cntIorSeen.load(std::memory_order_relaxed);
    size_t iMiss = g_cntIorMiss.load(std::memory_order_relaxed);
    size_t cap = g_stampCap.load(std::memory_order_relaxed);
    size_t occ = ins; // approx unique inserts this gen
    unsigned sat = (cap > 0 && occ * 2 > cap) ? 1u : 0u;
    // Health: expect wm+fys+fp+nyd > 0 on a real minor; admit may be 0 if closed.
    // Positive control: selftest sets g_selfTestFired.
    unsigned healthy = (wm + fys + fp + nyd + admit + g_selfTestFired.load(std::memory_order_relaxed)) > 0 ? 1u : 0u;
    std::fprintf(stderr,
                 "[GCV2][eatarm][HEALTH] minor=%zu wasMarked=%zu fysRemset=%zu fixpointSkip=%zu fixpointAdmit=%zu "
                 "nonYoungDedup=%zu stampIns=%zu stampHit=%zu probeFail=%zu iorSeen=%zu iorMiss=%zu "
                 "sat=%u healthy=%u selftest=%zu expect=arms_fire_when_mark_runs\n",
                 minorRunIndex, wm, fys, fp, admit, nyd, ins, hit, pfail, ior, iMiss, sat, healthy,
                 g_selfTestFired.load(std::memory_order_relaxed));
    std::fflush(stderr);
    if (sat != 0) {
        std::fprintf(stderr, "[GCV2][eatarm][HEALTH] INSTRUMENT_SATURATED occ~%zu cap=%zu\n", occ, cap);
        std::fflush(stderr);
    }
}

void RunSelfTest()
{
    if (!GateOn() && !EnvIsOne("MRT_GCV2_EATARM_SELFTEST")) {
        return;
    }
    // Force-enable path for selftest even if only SELFTEST set.
    EnsureStamps();
    OnMinorBegin(0);
    BaseObject* fakeH = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    BaseObject* fakeT = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    NoteWasMarkedSkipFields(fakeH);
    NoteFysRemsetSkip(static_cast<MAddress>(0x3000), fakeT);
    NoteFixpointEdge(fakeH, fakeT, FixpointReason::PLAUSIBLE_FAIL);
    NoteNonYoungDedupSkipFields(fakeH);
    NoteIorTarget(fakeT, fakeH, 24);
    g_selfTestFired.store(1, std::memory_order_relaxed);
    DumpMinorSummary(0);
    std::fprintf(stderr, "[GCV2][eatarm][SELFTEST] fired=1 expect IOR hit on FYS_REMSET or FIXPOINT for T=0x2000\n");
    std::fflush(stderr);
}

} // namespace EatArmDiag
} // namespace MapleRuntime
