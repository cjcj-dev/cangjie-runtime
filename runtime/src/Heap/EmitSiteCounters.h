// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Observational only (CANGJIE_GC_DEBUG_EQUIPMENT): per-barrier old→young / sticky counters.

#ifndef MRT_EMIT_SITE_COUNTERS_H
#define MRT_EMIT_SITE_COUNTERS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Common/BaseObject.h"

namespace MapleRuntime {

// Seven mutator barrier classes (task emitsite).
enum class EmitBarrierKind : int {
    Idle = 0,
    IdleLog = 1,
    Enum = 2,
    Trace = 3,
    PostTrace = 4,
    Preforward = 5,
    Forward = 6,
    COUNT = 7
};

// Per-barrier:
//   STORE_OLD_TO_YOUNG  — Write* saw holder old, ref young
//   STICKY_LOGGED       — that write also produced a sticky line attempt (IdleLog only)
//   STORE_ANY           — any Write* with heap holder (exercised proof)
//   STORE_HOLDER_YOUNG_REF_YOUNG — write-time holder still young, ref young (H2 candidates)
class EmitSiteCounters {
public:
    static void NoteWrite(EmitBarrierKind kind, BaseObject* holder, BaseObject* ref, bool stickyLogged);
    static void NoteStickyLogLine(BaseObject* object, bool newlyLogged);
    static void Dump(const char* tag);
    static void Reset();

    static constexpr int K = static_cast<int>(EmitBarrierKind::COUNT);

    static std::atomic<uint64_t> storeAny[K];
    static std::atomic<uint64_t> storeOldToYoung[K];
    static std::atomic<uint64_t> stickyLogged[K];
    static std::atomic<uint64_t> storeHolderYoungRefYoung[K];
    // write-time holderYoungAge histogram for old→young (age 0 expected for true old)
    static std::atomic<uint64_t> oldToYoungAge0[K];
    static std::atomic<uint64_t> oldToYoungAgeN[K]; // age > 0 while !IsYoungRegion (shouldn't)

    // CJ_MCC_StickyLogLine global
    static std::atomic<uint64_t> stickyLineCalls;
    static std::atomic<uint64_t> stickyLineNew;
    static std::atomic<uint64_t> stickyLineOnYoung;
    static std::atomic<uint64_t> stickyLineOnOld;
};

// Active barrier kind for inherited Write* (Idle/Preforward/Forward share IdleBarrier).
// InstallBarrier updates this; IdleLog/Enum/Trace/PostTrace pass kind explicitly.
inline std::atomic<int>& EmitSiteActiveKind()
{
    static std::atomic<int> active{static_cast<int>(EmitBarrierKind::Idle)};
    return active;
}

inline void EmitSiteSetActiveKind(EmitBarrierKind kind)
{
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteActiveKind().store(static_cast<int>(kind), std::memory_order_relaxed);
#else
    (void)kind;
#endif
}

// Convenience: classify holder/ref and bump counters. stickyLogged=true only when
// the barrier actually invokes LogObject / CJ_MCC_StickyLogLine for this write.
inline void EmitSiteNoteWrite(EmitBarrierKind kind, BaseObject* holder, BaseObject* ref, bool stickyLogged)
{
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteCounters::NoteWrite(kind, holder, ref, stickyLogged);
#else
    (void)kind;
    (void)holder;
    (void)ref;
    (void)stickyLogged;
#endif
}

// For IdleBarrier Write* inherited by Preforward/Forward: use active kind.
inline void EmitSiteNoteWriteActive(BaseObject* holder, BaseObject* ref, bool stickyLogged)
{
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    auto kind = static_cast<EmitBarrierKind>(EmitSiteActiveKind().load(std::memory_order_relaxed));
    EmitSiteCounters::NoteWrite(kind, holder, ref, stickyLogged);
#else
    (void)holder;
    (void)ref;
    (void)stickyLogged;
#endif
}

} // namespace MapleRuntime

#endif // MRT_EMIT_SITE_COUNTERS_H
