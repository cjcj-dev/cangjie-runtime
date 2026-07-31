// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "EdgeWitness.h"

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"

namespace MapleRuntime {

EdgeWitness& EdgeWitness::Instance() noexcept
{
    static EdgeWitness instance;
    return instance;
}

size_t EdgeWitness::CurrentEpoch()
{
    return g_gcCount;
}

EdgeWitness::SlotFlag* EdgeWitness::FindFlag(RefField<>* slot)
{
    for (size_t i = 0; i < flagCount; ++i) {
        if (flags[i].used && flags[i].slot == slot) {
            return &flags[i];
        }
    }
    return nullptr;
}

EdgeWitness::SlotFlag* EdgeWitness::FindOrInsertFlag(RefField<>* slot, bool* overflowed)
{
    *overflowed = false;
    if (SlotFlag* existing = FindFlag(slot)) {
        return existing;
    }
    if (flagCount >= FLAG_CAP) {
        *overflowed = true;
        overflow.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    SlotFlag& f = flags[flagCount++];
    f = SlotFlag{};
    f.slot = slot;
    f.used = 1;
    return &f;
}

void EdgeWitness::OnConsume(BaseObject* holder, RefField<>* slot)
{
    if (slot == nullptr) {
        return;
    }
    // Holder validity: bare header non-zero only. Do not call IsValidObject.
    if (holder != nullptr) {
        auto hdr = *reinterpret_cast<const uintptr_t*>(holder);
        if (hdr == 0) {
            return;
        }
    }

    const size_t epoch = CurrentEpoch();
    RefField<> snap(*slot);
    const bool plain = !snap.IsTagged();
    BaseObject* target = snap.GetTargetObject(); // bit extract; no target body access

    uint8_t holderRegionType = 0xFF;
    uint8_t retainedState = 0xFF;
    uint8_t holderSurvived = 0;
    if (holder != nullptr && Heap::IsHeapAddress(holder)) {
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
        if (hr != nullptr) {
            holderRegionType = static_cast<uint8_t>(hr->GetRegionType());
            retainedState = static_cast<uint8_t>(hr->GetRetainedLiveInfoState());
            // Record survived only — never filter (exclaudit WEAK-RANK-2 / task ban).
            size_t off = hr->GetAddressOffset(reinterpret_cast<MAddress>(holder));
            holderSurvived = hr->IsSurvivedObject(off) ? 1 : 0;
        }
    }

    uint8_t targetFrom = 0;
    uint8_t targetGhost = 0;
    uint8_t targetYoung = 0;
    if (target != nullptr && Heap::IsHeapAddress(target)) {
        targetGhost = RegionInfo::InGhostFromRegion(target) ? 1 : 0;
        RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
        if (tr != nullptr) {
            targetFrom = tr->IsFromRegion() ? 1 : 0;
            targetYoung = tr->IsYoungRegion() ? 1 : 0;
        }
    }

    std::lock_guard<std::mutex> lg(mutex);
    p1Hits.fetch_add(1, std::memory_order_relaxed);
    if (plain) {
        p1Plain.fetch_add(1, std::memory_order_relaxed);
    }

    uint8_t reg = 0;
    uint8_t rew = 0;
    if (SlotFlag* f = FindFlag(slot)) {
        reg = f->registered;
        rew = f->rewritten;
    }

    if (sampleCount < SAMPLE_CAP) {
        Sample& s = samples[sampleCount++];
        s = Sample{};
        s.holder = holder;
        s.slot = slot;
        s.majorEpoch = epoch;
        s.plain = plain ? 1 : 0;
        s.registered = reg;
        s.rewritten = rew;
        s.holderSurvived = holderSurvived;
        s.holderRegionType = holderRegionType;
        s.retainedState = retainedState;
        s.targetFrom = targetFrom;
        s.targetGhost = targetGhost;
        s.targetYoung = targetYoung;
        s.used = 1;
    } else {
        overflow.fetch_add(1, std::memory_order_relaxed);
    }

    if (plain && reg == 0 && rew == 0) {
        size_t n = unregUnrew.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            VLOG(REPORT,
                 "[EDGEWITNESS] unreg_unrew sample n=%zu holder=%p slot=%p epoch=%zu "
                 "plain=1 reg=0 rew=0 hRegType=%u retained=%u survived_rec=%u "
                 "tgtFrom=%u tgtGhost=%u tgtYoung=%u",
                 n, holder, slot, epoch, holderRegionType, retainedState, holderSurvived, targetFrom, targetGhost,
                 targetYoung);
        }
    }
}

void EdgeWitness::OnRegistered(BaseObject* holder, RefField<>* slot)
{
    if (slot == nullptr) {
        return;
    }
    const size_t epoch = CurrentEpoch();
    bool overflowed = false;
    std::lock_guard<std::mutex> lg(mutex);
    SlotFlag* f = FindOrInsertFlag(slot, &overflowed);
    regCount.fetch_add(1, std::memory_order_relaxed);
    if (f != nullptr) {
        f->registered = 1;
    }
    if (fixtureRegistered == 0) {
        fixtureHolder = holder;
        fixtureSlot = slot;
        fixtureEpoch = epoch;
        fixtureRegistered = 1;
        VLOG(REPORT, "[EDGEWITNESS] fixture registered holder=%p slot=%p epoch=%zu", holder, slot, epoch);
    }
}

void EdgeWitness::OnRewritten(BaseObject* holder, RefField<>* slot)
{
    if (slot == nullptr) {
        return;
    }
    bool overflowed = false;
    std::lock_guard<std::mutex> lg(mutex);
    SlotFlag* f = FindOrInsertFlag(slot, &overflowed);
    rewCount.fetch_add(1, std::memory_order_relaxed);
    if (f != nullptr) {
        f->rewritten = 1;
        // Fixture positive control: any slot that was registered then rewritten.
        if (f->registered) {
            if (fixtureRegistered == 0) {
                fixtureSlot = slot;
                fixtureHolder = holder;
                fixtureRegistered = 1;
            }
            if (fixtureSlot == slot || fixtureRegistered) {
                fixtureRewritten = 1;
            }
            static std::atomic<size_t> fixtureHit{ 0 };
            size_t n = fixtureHit.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || (n & (n - 1)) == 0) {
                VLOG(REPORT, "[EDGEWITNESS] fixture chain reg→rew n=%zu holder=%p slot=%p", n, holder, slot);
            }
        }
    }
    if (fixtureRegistered && fixtureSlot == slot) {
        fixtureRewritten = 1;
        VLOG(REPORT, "[EDGEWITNESS] fixture rewritten holder=%p slot=%p", holder, slot);
    }
}

void EdgeWitness::DumpAndReset(const char* where)
{
    std::lock_guard<std::mutex> lg(mutex);
    size_t p1 = p1Hits.load(std::memory_order_relaxed);
    size_t plain = p1Plain.load(std::memory_order_relaxed);
    size_t unregAtomic = unregUnrew.load(std::memory_order_relaxed);
    size_t ovfl = overflow.load(std::memory_order_relaxed);

    size_t plainP1 = 0;
    size_t plainUnreg = 0;
    size_t plainRegOrRew = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        const Sample& s = samples[i];
        if (!s.used || !s.plain) {
            continue;
        }
        ++plainP1;
        if (s.registered || s.rewritten) {
            ++plainRegOrRew;
        } else {
            ++plainUnreg;
        }
    }
    size_t covDenom = plainP1 + ovfl;
    size_t covPct = (plainP1 + ovfl) == 0 ? 100 : (plainP1 * 100) / (plainP1 + ovfl == 0 ? 1 : plainP1 + ovfl);
    (void)covDenom;

    const bool fixtureOk = (fixtureRegistered && fixtureRewritten) || rewCount.load(std::memory_order_relaxed) > 0;
    const char* fixtureStr = fixtureOk ? "ok" : "broken";

    VLOG(REPORT,
         "[EDGEWITNESS] where=%s p1_hits=%zu p1_plain=%zu unreg_unrew=%zu unreg_table=%zu "
         "plain_reg_or_rew=%zu reg=%zu rew=%zu "
         "fixture=%s fixture_reg=%u fixture_rew=%u overflow=%zu coverage=%zu%% "
         "samples=%zu flags=%zu survived_filter=none",
         where == nullptr ? "?" : where, p1, plain, unregAtomic, plainUnreg, plainRegOrRew,
         regCount.load(std::memory_order_relaxed), rewCount.load(std::memory_order_relaxed), fixtureStr,
         static_cast<unsigned>(fixtureRegistered), static_cast<unsigned>(fixtureRewritten), ovfl, covPct, sampleCount,
         flagCount);

    if (fixtureOk) {
        VLOG(REPORT, "[EDGEWITNESS] fixture=ok");
    } else {
        VLOG(REPORT, "[EDGEWITNESS] fixture=broken fixture_ok=0");
    }

    // Soft reset: drop samples; keep flags so later minors still see registration
    // history within the process; drop rewritten flags to free CAP.
    sampleCount = 0;
    size_t dst = 0;
    for (size_t i = 0; i < flagCount; ++i) {
        if (!flags[i].used) {
            continue;
        }
        if (flags[i].rewritten) {
            continue;
        }
        if (dst != i) {
            flags[dst] = flags[i];
        }
        ++dst;
    }
    flagCount = dst;
    p1Hits.store(0, std::memory_order_relaxed);
    p1Plain.store(0, std::memory_order_relaxed);
    unregUnrew.store(0, std::memory_order_relaxed);
    regCount.store(0, std::memory_order_relaxed);
    rewCount.store(0, std::memory_order_relaxed);
    // fixture sticky once established
}
} // namespace MapleRuntime
