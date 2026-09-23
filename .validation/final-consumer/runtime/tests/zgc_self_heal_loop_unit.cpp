// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Positive control for the ported OpenJDK ZZBarrier::self_heal loop
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

#include "ObjectModel/RefField.h"

using MapleRuntime::HeapSlotAt;
using MapleRuntime::MAddress;
using MapleRuntime::raw;
using MapleRuntime::zpointer;

template<typename FastPath>
void ProbeSelfHeal(MapleRuntime::HeapSlot<false>& slot, zpointer ptr, zpointer healPtr, FastPath fast_path,
                   bool allow_null)
{
    if (!allow_null && MapleRuntime::is_null_any(healPtr) && !MapleRuntime::is_null_any(ptr)) {
        return;
    }
    if (fast_path(ptr) || !fast_path(healPtr)) {
        return;
    }
    for (;;) {
        zpointer prev = zpointer::null;
        if (slot.CompareExchange(ptr, healPtr, std::memory_order_relaxed, std::memory_order_relaxed, &prev)) {
            return;
        }
        if (fast_path(prev)) {
            return;
        }
        ptr = prev;
    }
}

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
    ScriptedFastPath(MAddress* word, MAddress additionalGoodValue, MAddress bumpTo)
        : word(word), additionalGoodValue(additionalGoodValue), bumpTo(bumpTo)
    {}

    bool operator()(zpointer value)
    {
        const MAddress observed = static_cast<MAddress>(raw(value));
        // Every test heal value models make_load_good's proven result. The
        // optional second value scripts zBarrier.inline.hpp:98-101, where a
        // competing barrier has already installed another accepted value.
        if (observed == kHeal || (additionalGoodValue != 0 && observed == additionalGoodValue)) {
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
    MAddress additionalGoodValue;
    MAddress bumpTo;
};

// zBarrier.inline.hpp:91-96 -- uncontended CAS. This is the only arm survival_dense
// reaches, so it is the control for the two below.
void CaseUncontended()
{
    MAddress word = kPtr;
    auto slot = HeapSlotAt<false>(&word);
    ProbeSelfHeal(slot, MapleRuntime::to_zpointer(kPtr), MapleRuntime::to_zpointer(kHeal),
                  ScriptedFastPath(&word, 0, 0), false);
    Expect("uncontended -> healed", word, kHeal);
}

// zBarrier.inline.hpp:103-107 -- "The oop location was healed by another barrier, but
// still needs upgrading." The bounded loop cannot reach this: on a lost CAS it
// re-resolves from scratch and, after kSelfHealAttempts, returns without writing.
void CaseLostCasThenUpgrade()
{
    MAddress word = kOther1;
    auto slot = HeapSlotAt<false>(&word);
    ProbeSelfHeal(slot, MapleRuntime::to_zpointer(kPtr), MapleRuntime::to_zpointer(kHeal),
                  ScriptedFastPath(&word, 0, kOther2), false);
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
    auto slot = HeapSlotAt<false>(&word);
    ProbeSelfHeal(slot, MapleRuntime::to_zpointer(kPtr), MapleRuntime::to_zpointer(kHeal),
                  ScriptedFastPath(&word, kOther1, 0), false);
    Expect("fast-path prev -> no write", word, kOther1);
}

// zBarrier.inline.hpp:73-79 -- never heal a non-null reference with null.
void CaseNullHealRefused()
{
    MAddress word = kPtr;
    auto slot = HeapSlotAt<false>(&word);
    ProbeSelfHeal(slot, MapleRuntime::to_zpointer(kPtr), zpointer::null, ScriptedFastPath(&word, 0, 0), false);
    Expect("null heal -> refused", word, kPtr);
}

} // namespace

int main()
{
    CaseUncontended();
    CaseLostCasThenUpgrade();
    CaseFastPathExit();
    CaseNullHealRefused();
    std::printf("ZGC_SELF_HEAL_LOOP_UNIT %s failures=%u\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
