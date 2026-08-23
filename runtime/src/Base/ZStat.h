// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZSTAT_H
#define MRT_ZSTAT_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MapleRuntime {
// Two-level gate (0823 二轮): a default-OFF runtime env check still costs ~4ns per Timer scope
// (measured, kkk2 20M-op pairs) and that was enough to push the natural_wave gold cliff at 320MB
// up by one heap step.  So the first level is compile-time, same shape as kGcTrigger* in
// GcTriggerFlags.h: a product build without -DMRT_ZSTAT (cmake option, default OFF) contains no
// ZStat code at all -- every entry point below is an inline no-op and `nm -D` finds zero ZStat
// symbols.  The second level (only in builds that compiled it in) is the MRT_ZSTAT env var.
#ifndef MRT_ZSTAT_COMPILED
#define MRT_ZSTAT_COMPILED 0
#endif

#if MRT_ZSTAT_COMPILED

// Port of ZGC's ZStatPhase family (zStat.hpp:212-342): every GC phase is timed and its duration is
// booked under exactly one of two kinds -- pause or concurrent -- so a consumer never has to guess
// whether a number contains the concurrent window.  ZGC binds the kind statically to each phase
// object (ZStatPhasePause zStat.hpp:257 / ZStatPhaseConcurrent zStat.hpp:270) because no HotSpot
// phase name is ever used in both contexts.  This runtime reuses the same MRT_PHASE_TIMER name on
// both sides of a world-release (e.g. "young.ref_fix_bulk" runs under STW1 and again concurrently),
// so a static kind would misclassify half its samples.  The kind is therefore sampled at scope
// entry from the STW depth counter kept by EnterStwScope/ExitStwScope (driven by
// ScopedStopTheWorld), which classifies each individual sample rather than each name.
//
// The registry property of ZStatValue (zStat.hpp:66 -- "which counters exist" is an enumerable
// fact, not a grep result) is kept: every observed phase name registers on first sight and
// RegisteredPhases() enumerates the set.
//
// Everything is gated by MRT_ZSTAT (default off).  When off, NotePhase is never called (Timer
// skips it), the STW depth counter short-circuits on a cached bool, and NoteCycleEnd returns
// before touching anything -- the default-path rec=cycle/rec=phase/rec=stw stream is unchanged.
class ZStat {
public:
    struct PhaseTotals {
        uint64_t pauseNs = 0;    // sum of samples that started with the world stopped
        uint64_t concNs = 0;     // sum of samples that started with the world running
        uint64_t maxPauseNs = 0; // ZStatPhasePause::_max analog (zStat.cpp:750)
        uint32_t nPause = 0;
        uint32_t nConc = 0;
    };

    static bool Enabled();

    // STW depth, maintained by ScopedStopTheWorld.  Kind at scope entry is depth>0.
    static void EnterStwScope();
    static void ExitStwScope();
    static bool WorldStoppedNow();

    // Called from ~Timer only when Enabled().  ns is the scope duration; worldStoppedAtStart was
    // sampled in the Timer ctor so a phase that straddles the world-release is classified by where
    // its work began.
    static void NotePhase(const char* name, bool worldStoppedAtStart, uint64_t ns);

    // Cycle rollup, called next to GcLog::Cycle with the same seq.  Emits one
    //   [ZSTAT] v=1 rec=zphase seq= name= pause_ns= conc_ns= n=
    // per registered phase and one
    //   [ZSTAT] v=1 rec=zcycle seq= pause_ns= conc_ns= max_pause_ns= phases=
    // then resets the per-cycle table.  No-op when disabled.
    static void NoteCycleEnd(uint64_t seq);

    // Introspection for consumers and the unit suite.  These read the live table regardless of
    // the env gate so a test can drive NotePhase directly.
    static PhaseTotals Phase(const char* name);
    static std::vector<std::string> RegisteredPhases();
    static uint64_t CyclePauseNs();
    static uint64_t CycleConcNs();
    static uint64_t CycleMaxPauseNs();

    // Test hooks: the env gate is cached, so a test process flips the override instead.
    static void SetEnabledForTest(bool enabled);
    static void ResetForTest();

private:
    struct Table {
        std::unordered_map<std::string, PhaseTotals> phases;
        uint64_t pauseNs = 0;
        uint64_t concNs = 0;
        uint64_t maxPauseNs = 0;
    };

    static void EmitLine(const char* format, ...);
    // One token per value (same folding rule as GcLog::FoldToToken) so key=value readers hold.
    static void FoldToToken(const char* text, char* out, size_t cap);

    static std::mutex& TableLock();
    static Table& CycleTable();

    static std::atomic<int> g_stwDepth;
    static std::atomic<int> g_enabledOverride; // -1 = read env, 0/1 = forced by SetEnabledForTest
};

#else // !MRT_ZSTAT_COMPILED: every entry point is an inline no-op; the build contains no ZStat code.

class ZStat {
public:
    static constexpr bool Enabled() { return false; }
    static void EnterStwScope() {}
    static void ExitStwScope() {}
    static constexpr bool WorldStoppedNow() { return false; }
    static void NotePhase(const char*, bool, uint64_t) {}
    static void NoteCycleEnd(uint64_t) {}
};

#endif // MRT_ZSTAT_COMPILED
} // namespace MapleRuntime
#endif // MRT_ZSTAT_H
