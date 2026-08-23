// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Positive control for the ported OpenJDK ZBarrier::self_heal loop
// (ObjectModel/RefField.h, from zBarrier.inline.hpp:72-110).
//
// Why this exists: survival_dense exercises the port 56 times per run and every
// one of those CASes lands on the first attempt. That proves the entry, the CAS
// and the monotonicity check run -- and leaves the two arms that make the port
// different from the bounded kSelfHealAttempts loop (:98-101 fast-path exit and
// :103-107 upgrade retry) completely cold. A cold arm and an absent arm look the
// same in a census, so drive them here instead.
//
// The race is scripted rather than threaded: the ZBarrierFastPath is a functor
// this test owns, and it writes the slot as a side effect, which is exactly what
// "another barrier got there first" looks like to the CAS on the next iteration.
//
// Nothing here is dereferenced. The words only have to satisfy HeapSlot's bit
// layout (address in 0..47), so no heap, no collector and no GC phase are needed.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Heap/Verify/ZgcSelfHealDiag.h"
#include "ObjectModel/RefField.h"

using MapleRuntime::HealSite;
using MapleRuntime::HeapSlotAt;
using MapleRuntime::MAddress;
using MapleRuntime::raw;
using MapleRuntime::zpointer;

namespace {

constexpr MAddress kPtr = 0x0000700000001000ULL;    // what the barrier observed
constexpr MAddress kOther1 = 0x0000700000002000ULL; // what another writer left
constexpr MAddress kOther2 = 0x0000700000003000ULL; // and again, one iteration later
constexpr MAddress kHeal = 0x0000700000009000ULL;   // the value being healed in

unsigned g_failures = 0;

void Expect(const char* what, MAddress got, MAddress want)
{
    if (got == want) {
        std::printf("ok   %-28s word=%#llx\n", what, static_cast<unsigned long long>(got));
        return;
    }
    std::printf("FAIL %-28s word=%#llx want=%#llx\n", what, static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
    ++g_failures;
}

// The barrier's ZBarrierFastPath, under test control.
//   goodValue  the one word this fast path accepts (0 = accept nothing)
//   bumpTo     one-shot: when asked about the word currently in the slot, overwrite
//              the slot first. That makes the next CAS lose deterministically.
class ScriptedFastPath {
public:
    ScriptedFastPath(MAddress* word, MAddress goodValue, MAddress bumpTo)
        : word(word), goodValue(goodValue), bumpTo(bumpTo)
    {}

    bool operator()(zpointer value)
    {
        const MAddress observed = static_cast<MAddress>(raw(value));
        if (goodValue != 0 && observed == goodValue) {
            return true;
        }
        if (bumpTo != 0 && observed == *word) {
            *word = bumpTo;
            bumpTo = 0;
        }
        return false;
    }

private:
    MAddress* word;
    MAddress goodValue;
    MAddress bumpTo;
};

// zBarrier.inline.hpp:91-96 -- uncontended CAS. This is the only arm survival_dense
// reaches, so it is the control for the two below.
void CaseUncontended()
{
    MAddress word = kPtr;
    MapleRuntime::ZgcSelfHeal(HeapSlotAt<false>(&word), MapleRuntime::to_zpointer(kPtr),
                              MapleRuntime::to_zpointer(kHeal),
                              ScriptedFastPath(&word, 0, 0), HealSite::TraceReadReference);
    Expect("uncontended -> healed", word, kHeal);
}

// zBarrier.inline.hpp:103-107 -- "The oop location was healed by another barrier, but
// still needs upgrading." The bounded loop cannot reach this: on a lost CAS it
// re-resolves from scratch and, after kSelfHealAttempts, returns without writing.
void CaseLostCasThenUpgrade()
{
    MAddress word = kOther1;
    MapleRuntime::ZgcSelfHeal(HeapSlotAt<false>(&word), MapleRuntime::to_zpointer(kPtr),
                              MapleRuntime::to_zpointer(kHeal),
                              ScriptedFastPath(&word, 0, kOther2), HealSite::TraceReadReference);
    // Two lost CASes (kOther1 then kOther2), then the third lands. The point is not
    // that it retried -- it is that the slot ends on the heal value rather than on
    // whatever the other writer left.
    Expect("lost CAS -> still upgraded", word, kHeal);
}

// zBarrier.inline.hpp:98-101 -- "Must not self heal": another barrier already left the
// slot in a state this barrier's own fast path accepts, so the heal is abandoned and
// the slot keeps the other value.
void CaseFastPathExit()
{
    MAddress word = kOther1;
    MapleRuntime::ZgcSelfHeal(HeapSlotAt<false>(&word), MapleRuntime::to_zpointer(kPtr),
                              MapleRuntime::to_zpointer(kHeal),
                              ScriptedFastPath(&word, kOther1, 0), HealSite::TraceReadReference);
    Expect("fast-path prev -> no write", word, kOther1);
}

// zBarrier.inline.hpp:73-79 -- never heal a non-null reference with null.
void CaseNullHealRefused()
{
    MAddress word = kPtr;
    MapleRuntime::ZgcSelfHeal(HeapSlotAt<false>(&word), MapleRuntime::to_zpointer(kPtr),
                              zpointer::null, ScriptedFastPath(&word, 0, 0),
                              HealSite::TraceReadReference);
    Expect("null heal -> refused", word, kPtr);
}

// A fast path that never accepts anything and installs a *fresh* word every time it is
// asked. That is the shape of the case the loop has no defence against: a competing
// write that is not an upgrade, arriving faster than the heal can land.
class NeverConvergesFastPath {
public:
    NeverConvergesFastPath(MAddress* word, unsigned budget) : word(word), budget(budget) {}

    bool operator()(zpointer value)
    {
        const MAddress observed = static_cast<MAddress>(raw(value));
        if (budget > 0 && observed == *word) {
            ++served;
            *word = kOther1 + static_cast<MAddress>(served) * 0x1000ULL;
            --budget;
        }
        return false;
    }

private:
    MAddress* word;
    unsigned budget;
    unsigned served = 0;
};

// The livelock sentinel (ZgcSelfHealDiag kSpinAlarmIterations). ⛔ Not a ZGC arm:
// ZGC has no spin assert because the state is unreachable there. Budget is exactly
// the alarm threshold, so the alarm must fire once and the heal must still land
// afterwards -- the sentinel reports, it does not break the loop.
void CaseSpinAlarm(unsigned threshold)
{
    MAddress word = kOther1;
    MapleRuntime::ZgcSelfHeal(HeapSlotAt<false>(&word), MapleRuntime::to_zpointer(kPtr),
                              MapleRuntime::to_zpointer(kHeal),
                              NeverConvergesFastPath(&word, threshold),
                              HealSite::TraceReadReference);
    Expect("spin alarm -> loop survives", word, kHeal);
}

} // namespace

int main(int argc, char** argv)
{
    // Two modes, separate processes: the spin case moves retry by four figures and would
    // bury the arm counts the default mode exists to pin down.
    const bool spinMode = argc > 1 && std::strcmp(argv[1], "spin") == 0;

    if (spinMode) {
        CaseSpinAlarm(1024);
        // Expected census: enter=1 healed=1 retry=1025 iter_max=1025 spin_alarm=1.
        // 1025 because the competing write lands when the fast path is asked about the
        // prev value, i.e. after that iteration's CAS already read the slot.
        MapleRuntime::ZgcSelfHealDiag::Report("unit-spin");
    } else {
        CaseUncontended();
        CaseLostCasThenUpgrade();
        CaseFastPathExit();
        CaseNullHealRefused();
        // Expected census: enter=3 null_skip=1 healed=2 fastpath_exit=1 retry=2 iter_max=2
        // spin_alarm=0. The runner greps it; the arm counts are the point of the probe.
        MapleRuntime::ZgcSelfHealDiag::Report("unit");
    }

    std::printf("ZGC_SELF_HEAL_LOOP_UNIT %s failures=%u mode=%s\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures, spinMode ? "spin" : "default");
    return g_failures == 0 ? 0 : 1;
}
