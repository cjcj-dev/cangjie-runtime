// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Mutator self-relocation follows the ZGC retain/copy/wait protocol below.
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
// ZGC's corresponding path:
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
// WCollector::relocate_or_remap_object reaches the same point and must take
// these same three exits. Correctness is unconditional; counters only observe
// which exit supplied the forwarding receipt.
//
// The pin is NOT a new mechanism. RegionInfo::metadata.rwLock already is ZGC's _ref_count and
// _ref_lock in one object: a read lock is a retain (many holders, each visible in lockCount),
// and RwLock::LockWrite spins until lockCount reaches 0 -- that is detach_page's "wait until
// released". TryLockReadFromRegion (RegionInfo.h:2115) is the retain and is already used as
// one by TryForwardObject (WCollector.cpp:9012). The piece we were missing is the third one:
// no retire edge waits for readers. DispelGhostFromRegion is an unconditional bit-flip loop
// and TakeRegion's garbage reuse calls ClearUnits with no lock held at all.
namespace MutatorRelocate {

// zRelocate.cpp:382-416: find() miss after retain_page failed means the worker
// holds the page and is copying — wait. A published page without an object
// receipt violates the relocation invariant; there is no from-address answer.
enum class UnpublishedAnswer : uint32_t {
    UseTo = 0,
    Wait = 1,
    InvariantFailure = 2,
};

inline UnpublishedAnswer AnswerUnpublished(bool tableHit, bool regionPublished, bool retainRefused)
{
    if (tableHit) {
        return UnpublishedAnswer::UseTo;
    }
    if (regionPublished) {
        return UnpublishedAnswer::InvariantFailure;
    }
    // !published: region has not reached FORWARDED/COMPACTED/kept this cycle.
    // zRelocate.cpp:382-410 retains and completes the copy, or waits for the
    // current copier. retainRefused is one reason to wait, not an alternative
    // result.
    (void)retainRefused;
    return UnpublishedAnswer::Wait;
}

// ForwardObjectImpl LOCKED wait (WCollector.cpp). zRelocate.cpp:386-389 find() hit
// is "already relocated" and does not wait for the copier's lock; insert is the
// publish (zRelocate.cpp:371) and precedes UnlockObject(FORWARDED). A yield-only
// loop on IsLockedWord hung gc-main at the old :9570 while the mutator sat in
// SuspendForSync (REPORT-llstore hang_live). Page is_done (zForwarding.cpp:138-151)
// is the same exit as a published region: leftover LOCKED is not a live copier.
enum class LockedWaiterAnswer : uint32_t {
    UseTo = 0,
    Yield = 1,
    InvariantFailure = 2,
};

inline LockedWaiterAnswer AnswerLockedWaiter(bool tableHit, bool pagePublished)
{
    if (tableHit) {
        return LockedWaiterAnswer::UseTo;
    }
    if (pagePublished) {
        return LockedWaiterAnswer::InvariantFailure;
    }
    return LockedWaiterAnswer::Yield;
}

// Which forwarding retirement edge drained.
enum class Retire : uint32_t {
    DISPEL_GHOST = 0, // RegionInfo::DispelGhostFromRegion
    TAKE_GARBAGE = 1, // RegionManager::TakeRegion garbage reuse, around ClearUnits
    RECLAIM_DIRTY = 2,
    RECLAIM_MARK_QUARANTINE = 3,
    RELEASE_REGION = 4,
    RETIRE_COUNT = 5
};

// Why the mutator did not end up relocating. Reported per reason so a zero self_copy count
// says which leg swallowed it rather than just "it did not happen".
enum class Fallback : uint32_t {
    RETAIN_FAILED = 0, // TryLockReadFromRegion refused: write-locked, or no longer a from-region
    COPY_FAILED = 1,   // ForwardObjectImpl returned null (no route / gate rejected)
    PHASE = 2,         // not in PREFORWARD/FORWARD, so ForwardObjectImpl's CHECK would fire
    FALLBACK_COUNT = 3
};

// Counters observe the unconditional relocate path.
bool StatsOn();

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

// --- the leg we are trying to displace, counted in BOTH arms (gate: StatsOn) ------------
void NoteWaitEnter();   // entered WaitRoutedTipReady
void NoteWaitGiveUp();  // completed without a receipt and entered fail-closed handling
void NoteWaitReceipt(); // left it with a to-version
void NoteWaitFatal();   // reached the permanentHole CHECK_DETAIL
// Region-level publish wait (oraclecut §4). Distinct from the retain-refused
// object-FORWARDED spin: this one waits for FORWARDED/COMPACTED/kept.
void NoteRegionWaitEnter();
void NoteRegionWaitGot();            // published and table hit
void NoteRegionWaitPublishedMiss();  // published, table miss -> invariant failure

// --- pin / drain ------------------------------------------------------------------------
void NoteDrain(Retire site, uint64_t spunNanos, bool contended);

void DumpSummary();

} // namespace MutatorRelocate
} // namespace MapleRuntime

#endif // MRT_MUTATOR_RELOCATE_H
