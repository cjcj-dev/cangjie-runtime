// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// cjpmnull3: NoteAttempt / NoteRetainOk / NoteSelfCopy / atexit DumpSummary restored
// (PermWhoAdmit.cpp shape). Enabled() is still kMutatorSelfRelocate — counters do not
// turn the leg off. A zero attempt with funnel_mut>0 would mean the remap path never
// reached retain; a non-zero retain is the positive that self-relocate is live.
#ifndef MRT_MUTATOR_RELOCATE_H
#define MRT_MUTATOR_RELOCATE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// portmutreloc: port of ZRelocate::relocate_object (zRelocate.cpp:382-410) -- the mutator
// relocates the object on its own thread instead of waiting for a GC worker -- together with
// the from-page pin that path depends on (ZForwarding::retain_page / release_page /
// detach_page, zForwarding.cpp:86-108 / :134-169 / :171-181).
//
// What ZGC does, and what we did instead:
//
//   ZRelocate::relocate_object(forwarding, from_addr):
//       to = forwarding->find(from_addr, &cursor);      // pure table lookup
//       if (to) return to;                              // already relocated
//       if (forwarding->retain_page(&_queue)) {         // pin the FROM page
//           to = relocate_object_inner(...);            // *** copies it itself ***
//           forwarding->release_page();
//           if (to) return to;
//           _queue.add_and_wait(forwarding);            // only now wait for a worker
//       }
//       return forward_object(forwarding, from_addr);
//
// WCollector::relocate_or_remap_object (WCollector.h) reaches the same point -- a route
// exists, the object is not FORWARDED yet -- and has no "copy it yourself" leg at all. It
// either hands the mutator the *from* pointer back (the `!IsLockedState()` arm) or spins in
// WaitRoutedTipReady for up to kMaxSpins=4096 and then takes a FATAL (WCollector.cpp:8816,
// :8885). This unit adds the missing leg, behind a gate.
//
// The two halves are separately observable because they answer different questions:
//
//   MRT_GCV2_MUTATOR_RELOCATE=0   Off-switch. Default on (zconc / ZGC relocate_object).
//                                 Drain is unconditional via ZForwardingLife.
//   MRT_GCV2_MUTRELOC_DRAIN=1     The pin's drain half only (detach_page), with no mutator
//                                 relocation. Lets the drain be measured on its own.
//   MRT_GCV2_MUTRELOC_STATS=1     Counters only, no behaviour change. This is what makes the
//                                 off-arm readable: the wait-leg counters are gated on this
//                                 rather than on the feature, so "how often did we fall into
//                                 the 4096-spin wait" is measurable with the feature OFF and
//                                 the two arms can be compared. A feature-gated counter would
//                                 read 0 in the off arm for the trivial reason.
//
// The pin is NOT a new mechanism. RegionInfo::metadata.rwLock already is ZGC's _ref_count and
// _ref_lock in one object: a read lock is a retain (many holders, each visible in lockCount),
// and RwLock::LockWrite spins until lockCount reaches 0 -- that is detach_page's "wait until
// released". TryLockReadFromRegion (RegionInfo.h:2115) is the retain and is already used as
// one by TryForwardObject (WCollector.cpp:9012). The piece we were missing is the third one:
// no retire edge waits for readers. DispelGhostFromRegion is an unconditional bit-flip loop
// and TakeRegion's garbage reuse calls ClearUnits with no lock held at all.
namespace MutatorRelocate {

// Compile-time on. zRelocate.cpp:382-410: mutator copies on the spot; no MRT_GCV2_* gate.
constexpr bool kMutatorSelfRelocate = true;
// zRelocate.cpp:382-416: find() miss after retain_page failed means the worker
// holds the page and is copying — wait, do not keep from. Keep-from is only the
// VisitLive hole: page already published and the table still has no entry.
// ANALYSIS-crashoracle H1 / LEAD 0819-12:2x: (void)retainRefused handed out a
// naked from while ClearUnits ran on the same page.
constexpr bool kUnpublishedMeansKeepFrom = true;
constexpr int kInflightWaitSpins = 4096;
// ANALYSIS-crashoracle 第二轮 §3 CUT-1: load-good ⇒ 必是当前版本.
// keep-from 答案禁止 ZgcSelfHealLoadGood (zBarrier.inline.hpp:72-107 只 heal
// 已证明的当前地址). Invert this to restore the 1s crash (task ④).
constexpr bool kSkipHealOnTransient = true;

enum class UnpublishedAnswer : uint32_t {
    UseTo = 0,
    KeepFrom = 1,
    Wait = 2,
};

// Resolve-chain answer grade. proven-current may heal-good; transient
// (every keep-from) must not — slot stays load-bad so the next read
// re-enters the barrier (zGeneration.inline.hpp:131-140).
enum class ResolveGrade : uint32_t {
    ProvenCurrent = 0,
    Transient = 1,
};

inline UnpublishedAnswer AnswerUnpublished(bool tableHit, bool regionPublished, bool retainRefused)
{
    if (tableHit) {
        return UnpublishedAnswer::UseTo;
    }
    if (retainRefused) {
        return UnpublishedAnswer::Wait;
    }
    if (regionPublished) {
        return UnpublishedAnswer::KeepFrom;
    }
    return UnpublishedAnswer::KeepFrom;
}

// Which retire edge drained. Mirrors FwdInflight::Retire, which measured the same edges.
enum class Retire : uint32_t {
    DISPEL_GHOST = 0, // RegionInfo::DispelGhostFromRegion
    TAKE_GARBAGE = 1, // RegionManager::TakeRegion garbage reuse, around ClearUnits
    RETIRE_COUNT = 2
};

// Why the mutator did not end up relocating. Reported per reason so a zero self_copy count
// says which leg swallowed it rather than just "it did not happen".
enum class Fallback : uint32_t {
    RETAIN_FAILED = 0, // TryLockReadFromRegion refused: write-locked, or no longer a from-region
    COPY_FAILED = 1,   // ForwardObjectImpl returned null (no route / gate rejected)
    PHASE = 2,         // not in PREFORWARD/FORWARD, so ForwardObjectImpl's CHECK would fire
    FALLBACK_COUNT = 3
};

// Relocate leg on. Implies DrainEnabled().
bool Enabled();
// Drain half on (implied by Enabled(), or standalone via MRT_GCV2_MUTRELOC_DRAIN=1).
bool DrainEnabled();
// Counters on. True whenever either half is on, or standalone via MRT_GCV2_MUTRELOC_STATS=1.
bool StatsOn();
// Positive control (MRT_GCV2_MUTRELOC_INJECT=1). See MutatorRelocate.cpp for what it forces
// and why attempts=0 on its own cannot be read as either success or failure.
bool InjectOn();

// --- relocate leg ---------------------------------------------------------------------
void NoteAttempt();
void NoteRetainOk();
void NoteFallback(Fallback why);
// The mutator found the object already FORWARDED after retaining. Counted apart from
// self_copy: it means the leg ran but someone else had done the work.
void NoteAlreadyForwarded();

// Set for the duration of one mutator relocate attempt on this thread. ForwardObjectExclusive
// consults it so the copy is attributed to the thread that actually ran CopyObject -- the
// headline number. Without this, a count taken at the call site could not tell "this mutator
// copied the object" from "this mutator observed a copy a GC worker had already made".
bool InScope();
void EnterScope();
void LeaveScope();
// Which kind of thread ran a copy. ThreadType (ThreadLocal.h:20) is
// {CJ_PROCESSOR, GC_THREAD, FP_THREAD, HOT_UPDATE_THREAD}; IsRuntimeThread() is
// >= GC_THREAD, so a mutator is exactly !IsRuntimeThread(), i.e. CJ_PROCESSOR.
//
// This exists because "the ported leg performed the copy" and "a MUTATOR performed the copy"
// are different claims, and only the second one is what mutator relocation means. The first
// round of measurement could only support the first: the control routed through
// TryForwardObject, which is reachable from both mutator barriers and GC-side callers
// (RegionManager.cpp:2586/2732, Heap.cpp:372), so the copies it counted had no attributed
// thread role at all.
enum class Role : uint32_t {
    MUTATOR = 0,     // CJ_PROCESSOR -- the claim under test
    GC = 1,          // GC_THREAD
    OTHER_RT = 2,    // FP_THREAD / HOT_UPDATE_THREAD -- runtime, but not the collector
    ROLE_COUNT = 3
};

// Called from WCollector::ForwardObjectExclusive, immediately after CopyObject, when InScope().
void NoteSelfCopy(size_t bytes, Role role);
// Called from the same place for every copy regardless of thread. Separates "the ported leg
// lost every race" from "nothing was relocated in this run at all", and gives the denominator
// the per-role self_copies counts are a share of.
void NoteAnyCopy(Role role);

// Every entry into WCollector::relocate_or_remap_object, bucketed by role.
//
// This is the control for the role predicate itself. "role=mutator is absent from the copy
// census" has two readings -- no mutator ever relocated, or IsGcThread/IsRuntimeThread are
// not discriminating here -- and they call for opposite conclusions. The remap funnel is
// entered from make_load_good inside the six barriers, which user code runs, so a working
// predicate has to produce a non-zero mutator count here. If this census is also gc-only,
// the copy census says nothing about roles and must not be read as if it did.
void NoteFunnelCall(Role role);

// --- the leg we are trying to displace, counted in BOTH arms (gate: StatsOn) ------------
void NoteWaitEnter();   // entered WaitRoutedTipReady
void NoteWaitGiveUp();  // left it without a to-version (spin bound hit, or copy not started)
void NoteWaitReceipt(); // left it with a to-version
void NoteWaitFatal();   // reached the permanentHole CHECK_DETAIL -- the 4096-spin FATAL leg

// CUT-1 answer grade. Reset at remap entry; keep-from sets Transient.
void ResetResolveGrade();
void SetResolveGrade(ResolveGrade grade);
ResolveGrade CurrentResolveGrade();
void ClearLastFallback();
// KeepFrom 出口按 I1 COPY_FAILED / I2 PHASE / I3 其余 计数.
void NoteKeepFromExit();
void NoteHealSkipped();

// --- pin / drain ------------------------------------------------------------------------
void NoteDrain(Retire site, uint64_t spunNanos, bool contended);

void DumpSummary();

} // namespace MutatorRelocate
} // namespace MapleRuntime

#endif // MRT_MUTATOR_RELOCATE_H
