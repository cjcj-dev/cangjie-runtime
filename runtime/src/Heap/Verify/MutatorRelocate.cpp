// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MutatorRelocate.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace MutatorRelocate {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

constexpr size_t kRetireCount = static_cast<size_t>(Retire::RETIRE_COUNT);
constexpr size_t kFallbackCount = static_cast<size_t>(Fallback::FALLBACK_COUNT);
constexpr size_t kRoleCount = static_cast<size_t>(Role::ROLE_COUNT);

std::atomic<size_t> g_attempts{ 0 };
std::atomic<size_t> g_retainOk{ 0 };
std::atomic<size_t> g_alreadyForwarded{ 0 };
std::atomic<size_t> g_selfCopies{ 0 };
std::atomic<size_t> g_anyCopies{ 0 };
std::atomic<size_t> g_anyCopiesByRole[kRoleCount];
std::atomic<size_t> g_selfCopiesByRole[kRoleCount];
std::atomic<size_t> g_funnelByRole[kRoleCount];
std::atomic<size_t> g_selfCopyBytes{ 0 };
std::atomic<size_t> g_fallbacks[kFallbackCount];

std::atomic<size_t> g_waitEnter{ 0 };
std::atomic<size_t> g_waitGiveUp{ 0 };
std::atomic<size_t> g_waitReceipt{ 0 };
std::atomic<size_t> g_waitFatal{ 0 };

std::atomic<size_t> g_drains[kRetireCount];
std::atomic<size_t> g_drainsContended[kRetireCount];
std::atomic<uint64_t> g_drainNanos[kRetireCount];
std::atomic<uint64_t> g_drainNanosMax[kRetireCount];

std::atomic<bool> g_atexit{ false };

thread_local bool tl_inScope = false;

const char* RetireName(Retire retire)
{
    switch (retire) {
        case Retire::DISPEL_GHOST:
            return "dispel_ghost";
        case Retire::TAKE_GARBAGE:
            return "take_garbage";
        default:
            return "unknown";
    }
}

const char* RoleName(Role role)
{
    switch (role) {
        case Role::MUTATOR:
            return "mutator";
        case Role::GC:
            return "gc";
        case Role::OTHER_RT:
            return "other_rt";
        default:
            return "unknown";
    }
}

const char* FallbackName(Fallback why)
{
    switch (why) {
        case Fallback::RETAIN_FAILED:
            return "retain_failed";
        case Fallback::COPY_FAILED:
            return "copy_failed";
        case Fallback::PHASE:
            return "phase";
        default:
            return "unknown";
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        (void)std::atexit([]() { DumpSummary(); });
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        // Register the summary here, not only from the Note* helpers. The first arm run had
        // every counter at zero and therefore printed nothing at all, which reads exactly like
        // "the instrument is not in the binary" -- the one reading it must not be able to make.
        EnsureAtexit();
        if (EnvIsOne("MRT_GCV2_MUTATOR_RELOCATE")) {
            return true;
        }
        return DiagGate::TokenOn("mutreloc");
    }();
    return on;
}

bool InjectOn()
{
    // Positive control on the SAME binary. The mutator remap funnel is workload-dependent:
    // on a workload whose barrier traffic never reaches a ghost-from region, attempts=0 is
    // consistent with "wired correctly and never needed" AND with "not wired at all". Inject
    // routes TryForwardObject -- the pre-existing path that already retains, copies and
    // releases, and which does run -- through TryMutatorRelocate, so retain, the copy under
    // scope, and the attribution in ForwardObjectExclusive all have to fire. With inject on,
    // self_copies must exceed 0; with it off it must be 0 unless the funnel itself fired.
    // Inject changes which function performs the copy, not what the copy does.
    static const bool on = []() {
        EnsureAtexit();
        if (EnvIsOne("MRT_GCV2_MUTRELOC_INJECT")) {
            return true;
        }
        return DiagGate::TokenOn("mutrelocinject");
    }();
    return on;
}

bool DrainEnabled()
{
    // Drain is now unconditional (ZForwardingLife). The flag is gone; this stays
    // true so existing census lines still print.
    EnsureAtexit();
    return true;
}

bool StatsOn()
{
    static const bool on = []() {
        EnsureAtexit();
        if (EnvIsOne("MRT_GCV2_MUTRELOC_STATS")) {
            return true;
        }
        return DiagGate::TokenOn("mutrelocstats");
    }();
    return on || Enabled() || DrainEnabled();
}

void NoteAttempt()
{
    EnsureAtexit();
    g_attempts.fetch_add(1, std::memory_order_relaxed);
}

void NoteRetainOk() { g_retainOk.fetch_add(1, std::memory_order_relaxed); }

void NoteAlreadyForwarded() { g_alreadyForwarded.fetch_add(1, std::memory_order_relaxed); }

void NoteFallback(Fallback why)
{
    EnsureAtexit();
    size_t idx = static_cast<size_t>(why);
    if (idx < kFallbackCount) {
        g_fallbacks[idx].fetch_add(1, std::memory_order_relaxed);
    }
}

bool InScope() { return tl_inScope; }

void EnterScope() { tl_inScope = true; }

void LeaveScope() { tl_inScope = false; }

void NoteSelfCopy(size_t bytes, Role role)
{
    g_selfCopies.fetch_add(1, std::memory_order_relaxed);
    g_selfCopyBytes.fetch_add(bytes, std::memory_order_relaxed);
    size_t idx = static_cast<size_t>(role);
    if (idx < kRoleCount) {
        g_selfCopiesByRole[idx].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteAnyCopy(Role role)
{
    size_t idx = static_cast<size_t>(role);
    if (idx < kRoleCount) {
        g_anyCopiesByRole[idx].fetch_add(1, std::memory_order_relaxed);
    }
    // Every ForwardObjectExclusive copy, whoever ran it. Without this, self_copies=0 is two
    // different findings wearing the same face: "the ported leg lost every race to a GC
    // worker" and "nothing was relocated at all in this run". any_copies separates them.
    g_anyCopies.fetch_add(1, std::memory_order_relaxed);
}

void NoteFunnelCall(Role role)
{
    EnsureAtexit();
    size_t idx = static_cast<size_t>(role);
    if (idx < kRoleCount) {
        g_funnelByRole[idx].fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteWaitEnter()
{
    EnsureAtexit();
    g_waitEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteWaitGiveUp() { g_waitGiveUp.fetch_add(1, std::memory_order_relaxed); }

void NoteWaitReceipt() { g_waitReceipt.fetch_add(1, std::memory_order_relaxed); }

void NoteWaitFatal() { g_waitFatal.fetch_add(1, std::memory_order_relaxed); }

void NoteDrain(Retire site, uint64_t spunNanos, bool contended)
{
    EnsureAtexit();
    size_t idx = static_cast<size_t>(site);
    if (idx >= kRetireCount) {
        return;
    }
    g_drains[idx].fetch_add(1, std::memory_order_relaxed);
    if (contended) {
        g_drainsContended[idx].fetch_add(1, std::memory_order_relaxed);
    }
    g_drainNanos[idx].fetch_add(spunNanos, std::memory_order_relaxed);
    uint64_t prev = g_drainNanosMax[idx].load(std::memory_order_relaxed);
    while (spunNanos > prev &&
           !g_drainNanosMax[idx].compare_exchange_weak(prev, spunNanos, std::memory_order_relaxed)) {
    }
}

void DumpSummary()
{
    if (!StatsOn()) {
        return;
    }
    // self_copies is the headline: the number of objects a mutator thread copied on its own
    // thread, attributed inside ForwardObjectExclusive rather than at the call site. With the
    // feature off this whole block of counters is structurally zero -- that is not a control,
    // it is arithmetic. The comparable pair across arms is wait_enter / wait_fatal, which are
    // gated on StatsOn() and therefore counted in both arms.
    LOG(RTLOG_ERROR,
        "[GCV2][mutreloc] SUMMARY relocate=%u drain=%u attempts=%zu retain_ok=%zu "
        "self_copies=%zu self_copy_bytes=%zu already_forwarded=%zu any_copies=%zu inject=%u",
        static_cast<unsigned>(Enabled()), static_cast<unsigned>(DrainEnabled()),
        g_attempts.load(std::memory_order_relaxed), g_retainOk.load(std::memory_order_relaxed),
        g_selfCopies.load(std::memory_order_relaxed), g_selfCopyBytes.load(std::memory_order_relaxed),
        g_alreadyForwarded.load(std::memory_order_relaxed), g_anyCopies.load(std::memory_order_relaxed),
        static_cast<unsigned>(InjectOn()));
    LOG(RTLOG_ERROR,
        "[GCV2][mutreloc] WAITLEG wait_enter=%zu wait_receipt=%zu wait_giveup=%zu wait_fatal=%zu",
        g_waitEnter.load(std::memory_order_relaxed), g_waitReceipt.load(std::memory_order_relaxed),
        g_waitGiveUp.load(std::memory_order_relaxed), g_waitFatal.load(std::memory_order_relaxed));
    // The claim mutator relocation actually makes is self=mutator > 0. Reported next to the
    // per-role denominator so the share is visible rather than asserted.
    for (size_t i = 0; i < kRoleCount; ++i) {
        size_t any = g_anyCopiesByRole[i].load(std::memory_order_relaxed);
        size_t self = g_selfCopiesByRole[i].load(std::memory_order_relaxed);
        if (any == 0 && self == 0 && g_funnelByRole[i].load(std::memory_order_relaxed) == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[GCV2][mutreloc] role=%s any_copies=%zu self_copies=%zu funnel_calls=%zu",
            RoleName(static_cast<Role>(i)), any, self,
            g_funnelByRole[i].load(std::memory_order_relaxed));
    }
    for (size_t i = 0; i < kFallbackCount; ++i) {
        size_t n = g_fallbacks[i].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        LOG(RTLOG_ERROR, "[GCV2][mutreloc] fallback=%s n=%zu",
            FallbackName(static_cast<Fallback>(i)), n);
    }
    for (size_t i = 0; i < kRetireCount; ++i) {
        size_t n = g_drains[i].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        LOG(RTLOG_ERROR,
            "[GCV2][mutreloc] drain=%s events=%zu contended=%zu total_ns=%llu max_ns=%llu",
            RetireName(static_cast<Retire>(i)), n,
            g_drainsContended[i].load(std::memory_order_relaxed),
            static_cast<unsigned long long>(g_drainNanos[i].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_drainNanosMax[i].load(std::memory_order_relaxed)));
    }
}

} // namespace MutatorRelocate
} // namespace MapleRuntime
