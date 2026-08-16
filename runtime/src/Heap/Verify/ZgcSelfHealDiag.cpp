// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/ZgcSelfHealDiag.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/ColourMask.h"
#include "Common/ColourPredicates.h"

namespace MapleRuntime {
namespace ZgcSelfHealDiag {
namespace {

std::atomic<uint64_t> g_enter{ 0 };
std::atomic<uint64_t> g_nullSkip{ 0 };
std::atomic<uint64_t> g_healed{ 0 };
std::atomic<uint64_t> g_fastPathExit{ 0 };
std::atomic<uint64_t> g_retry{ 0 };
std::atomic<uint64_t> g_iterMax{ 0 };

// zBarrier.inline.hpp:83-86 entry asserts.
std::atomic<uint64_t> g_preIsFastPath{ 0 };    // assert(!fast_path(ptr))
std::atomic<uint64_t> g_preHealNotGood{ 0 };   // assert(fast_path(heal_ptr))
std::atomic<uint64_t> g_preHealNotRemapped{ 0 }; // assert(ZPointer::is_remapped(heal_ptr))

// zBarrier.inline.hpp:51-53 -- the good-state half of the monotonicity assert.
std::atomic<uint64_t> g_monoLoadGood{ 0 };
std::atomic<uint64_t> g_monoMarkGood{ 0 };
std::atomic<uint64_t> g_monoStoreGood{ 0 };
// zBarrier.inline.hpp:67-69 -- the marked-epoch half.
std::atomic<uint64_t> g_monoMarkedYoung{ 0 };
std::atomic<uint64_t> g_monoMarkedOld{ 0 };
std::atomic<uint64_t> g_monoFinalizable{ 0 };
std::atomic<uint64_t> g_monoChecks{ 0 };
std::atomic<uint64_t> g_monoSampleLogged{ 0 };

constexpr uint64_t kMonoSampleCap = 32;

bool ReadFlag(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool AbortOnViolation()
{
    static const bool on = []() { return ReadFlag("MRT_GCV2_ZGC_SELFHEAL_ABORT"); }();
    return on;
}

#define ZSH_LD(c) static_cast<unsigned long long>((c).load(std::memory_order_relaxed))

void DumpCensusRaw(const char* why)
{
    // Single fprintf + fflush, no heap and no locks, so the signal path can use it.
    std::fprintf(stderr,
                 "[GCV2][zgcselfheal][census] why=%s armed=%d "
                 "enter=%llu null_skip=%llu healed=%llu fastpath_exit=%llu retry=%llu "
                 "cas_fail=%llu iter_max=%llu "
                 "pre_ptr_fastpath=%llu pre_heal_not_fastpath=%llu pre_heal_not_remapped=%llu "
                 "mono_checks=%llu mono_load_good=%llu mono_mark_good=%llu mono_store_good=%llu "
                 "mono_marked_young=%llu mono_marked_old=%llu mono_finalizable=%llu\n",
                 why == nullptr ? "?" : why, Enabled() ? 1 : 0, ZSH_LD(g_enter), ZSH_LD(g_nullSkip),
                 ZSH_LD(g_healed), ZSH_LD(g_fastPathExit), ZSH_LD(g_retry),
                 ZSH_LD(g_fastPathExit) + ZSH_LD(g_retry), ZSH_LD(g_iterMax),
                 ZSH_LD(g_preIsFastPath), ZSH_LD(g_preHealNotGood), ZSH_LD(g_preHealNotRemapped),
                 ZSH_LD(g_monoChecks), ZSH_LD(g_monoLoadGood), ZSH_LD(g_monoMarkGood),
                 ZSH_LD(g_monoStoreGood), ZSH_LD(g_monoMarkedYoung), ZSH_LD(g_monoMarkedOld),
                 ZSH_LD(g_monoFinalizable));
    std::fflush(stderr);
}

void CrashCensusHandler(int sig)
{
    const char* tag = "signal";
    if (sig == SIGSEGV) {
        tag = "sigsegv";
    } else if (sig == SIGABRT) {
        tag = "sigabrt";
    } else if (sig == SIGBUS) {
        tag = "sigbus";
    }
    DumpCensusRaw(tag);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// The census must print even when every counter is zero: that is what makes the
// off arm a control rather than an absence of evidence. So atexit is registered
// from load time under either gate, while the crash handlers are installed lazily
// on first use -- SignalManager installs its own later, and taking that over at
// load time would change the crash path of a run that is not even exercising this
// code (ToverFailDiag.cpp:108-118 makes the same trade).
std::atomic<bool> g_atexitRegistered{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitRegistered.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void InstallOnce()
{
    static std::atomic<bool> installed{ false };
    bool expected = false;
    if (installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        EnsureAtexit();
        std::signal(SIGSEGV, CrashCensusHandler);
        std::signal(SIGABRT, CrashCensusHandler);
        std::signal(SIGBUS, CrashCensusHandler);
        DumpCensusRaw("armed");
    }
}

struct CensusArmer {
    CensusArmer()
    {
        if (CensusEnabled()) {
            EnsureAtexit();
            DumpCensusRaw("load");
        }
    }
};
const CensusArmer g_censusArmer{};

void NoteIterations(unsigned iterations)
{
    uint64_t want = static_cast<uint64_t>(iterations);
    uint64_t seen = g_iterMax.load(std::memory_order_relaxed);
    while (want > seen && !g_iterMax.compare_exchange_weak(seen, want, std::memory_order_relaxed)) {
    }
}

void NoteViolation(std::atomic<uint64_t>& counter, const char* which, uintptr_t oldRaw,
                   uintptr_t newRaw)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    if (g_monoSampleLogged.fetch_add(1, std::memory_order_relaxed) < kMonoSampleCap) {
        std::fprintf(stderr,
                     "[GCV2][zgcselfheal][non-monotonic] which=%s old=%#zx heal=%#zx "
                     "load_bad=%#zx mark_bad=%#zx store_bad=%#zx\n",
                     which, static_cast<size_t>(oldRaw), static_cast<size_t>(newRaw),
                     static_cast<size_t>(::g_cjLoadBadMask), static_cast<size_t>(::g_cjMarkBadMask),
                     static_cast<size_t>(::g_cjStoreBadMask));
        std::fflush(stderr);
    }
    if (AbortOnViolation()) {
        // ZGC's assert(), opt-in.
        std::abort();
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() { return ReadFlag("MRT_GCV2_ZGC_SELFHEAL"); }();
    return on;
}

bool CensusEnabled()
{
    static const bool on = []() { return Enabled() || ReadFlag("MRT_GCV2_ZGC_SELFHEAL_REPORT"); }();
    return on;
}

void Report(const char* why)
{
    if (!CensusEnabled()) {
        return;
    }
    DumpCensusRaw(why);
}

// OpenJDK ZBarrier::assert_transition_monotonicity, zBarrier.inline.hpp:40-70.
//
// "A self heal must always upgrade the address metadata bits in accordance with the
// metadata bits state machine."
//
// The predicate names map one for one onto ColourPredicates (the transcription of
// ZPointer, ColourPredicates.h:96-233); the only shape difference is that ours take
// the published bad mask explicitly instead of reading a global inside.
void CheckTransitionMonotonicity(zpointer oldPtr, zpointer healPtr)
{
    if (!Enabled()) {
        return;
    }
    InstallOnce();
    g_monoChecks.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t oldRaw = static_cast<uintptr_t>(raw(oldPtr));
    const uintptr_t newRaw = static_cast<uintptr_t>(raw(healPtr));
    const uintptr_t loadBad = static_cast<uintptr_t>(::g_cjLoadBadMask);
    const uintptr_t markBad = static_cast<uintptr_t>(::g_cjMarkBadMask);
    const uintptr_t storeBad = static_cast<uintptr_t>(::g_cjStoreBadMask);

    // :41-49
    const bool oldIsLoadGood = ColourPredicates::is_load_good(oldRaw, loadBad);
    const bool oldIsMarkGood = ColourPredicates::is_mark_good(oldRaw, loadBad, markBad);
    const bool oldIsStoreGood = ColourPredicates::is_store_good(oldRaw, loadBad, storeBad);

    const bool newIsLoadGood = ColourPredicates::is_load_good(newRaw, loadBad);
    const bool newIsMarkGood = ColourPredicates::is_mark_good(newRaw, loadBad, markBad);
    const bool newIsStoreGood = ColourPredicates::is_store_good(newRaw, loadBad, storeBad);

    // :51-53
    if (oldIsLoadGood && !newIsLoadGood) {
        NoteViolation(g_monoLoadGood, "load_good", oldRaw, newRaw);
    }
    if (oldIsMarkGood && !newIsMarkGood) {
        NoteViolation(g_monoMarkGood, "mark_good", oldRaw, newRaw);
    }
    if (oldIsStoreGood && !newIsStoreGood) {
        NoteViolation(g_monoStoreGood, "store_good", oldRaw, newRaw);
    }

    // :55-58  Null is good enough at this point. ZGC's is_null_any tests the address
    // bits, not the whole word, so a colour-only word counts as null here too.
    if (!ColourPredicates::has_address(newRaw)) {
        return;
    }

    // :60-66
    const bool oldIsMarkedYoung = ColourPredicates::is_marked_young(oldRaw, markBad);
    const bool oldIsMarkedOld = ColourPredicates::is_marked_old(oldRaw, markBad);
    const bool oldIsMarkedFinalizable = ColourPredicates::is_marked_finalizable(oldRaw, markBad);

    const bool newIsMarkedYoung = ColourPredicates::is_marked_young(newRaw, markBad);
    const bool newIsMarkedOld = ColourPredicates::is_marked_old(newRaw, markBad);
    const bool newIsMarkedFinalizable = ColourPredicates::is_marked_finalizable(newRaw, markBad);

    // :68-70
    if (oldIsMarkedYoung && !newIsMarkedYoung) {
        NoteViolation(g_monoMarkedYoung, "marked_young", oldRaw, newRaw);
    }
    if (oldIsMarkedOld && !newIsMarkedOld) {
        NoteViolation(g_monoMarkedOld, "marked_old", oldRaw, newRaw);
    }
    // ZGC's third arm accepts MarkedOld as an upgrade from Finalizable. Ours can only
    // ever be false today: kFinalizableWired is false and nothing publishes the bits
    // (ColourMask.h:100-126), so this counter is a wired-yet check, not a live one.
    if (oldIsMarkedFinalizable && !newIsMarkedFinalizable && !newIsMarkedOld) {
        NoteViolation(g_monoFinalizable, "marked_finalizable", oldRaw, newRaw);
    }
}

void NotePreconditions(bool ptrFastPath, bool healFastPath, zpointer healPtr)
{
    if (!Enabled()) {
        return;
    }
    InstallOnce();
    // :84  assert(!fast_path(ptr), "Invalid self heal")
    if (ptrFastPath) {
        g_preIsFastPath.fetch_add(1, std::memory_order_relaxed);
    }
    // :85  assert(fast_path(heal_ptr), "Invalid self heal")
    if (!healFastPath) {
        g_preHealNotGood.fetch_add(1, std::memory_order_relaxed);
    }
    // :87  assert(ZPointer::is_remapped(heal_ptr), "invariant")
    if (!ColourPredicates::is_remapped(static_cast<uintptr_t>(raw(healPtr)),
                                       static_cast<uintptr_t>(::g_cjLoadBadMask))) {
        g_preHealNotRemapped.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteEnter()
{
    if (!Enabled()) {
        return;
    }
    InstallOnce();
    g_enter.fetch_add(1, std::memory_order_relaxed);
}

void NoteNullSkip()
{
    if (!Enabled()) {
        return;
    }
    InstallOnce();
    g_nullSkip.fetch_add(1, std::memory_order_relaxed);
}

void NoteHealed(unsigned iterations)
{
    if (!Enabled()) {
        return;
    }
    g_healed.fetch_add(1, std::memory_order_relaxed);
    NoteIterations(iterations);
}

void NoteFastPathExit(unsigned iterations)
{
    if (!Enabled()) {
        return;
    }
    g_fastPathExit.fetch_add(1, std::memory_order_relaxed);
    NoteIterations(iterations);
}

void NoteRetry(unsigned iterations)
{
    if (!Enabled()) {
        return;
    }
    g_retry.fetch_add(1, std::memory_order_relaxed);
    NoteIterations(iterations + 1);
}

} // namespace ZgcSelfHealDiag
} // namespace MapleRuntime
