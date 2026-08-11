// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/ToverFailDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/BaseObject.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace ToverFailDiag {
namespace {

std::atomic<uint64_t> g_loadGoodFast{ 0 };
std::atomic<uint64_t> g_slowEnter{ 0 };
std::atomic<uint64_t> g_unmovableSkip{ 0 };
std::atomic<uint64_t> g_unmovableSkipFwd{ 0 };
std::atomic<uint64_t> g_unmovableSkipLogged{ 0 };
std::atomic<uint64_t> g_resolveEnter{ 0 };
std::atomic<uint64_t> g_resolveMoved{ 0 };
std::atomic<uint64_t> g_resolveKeepFrom{ 0 };
std::atomic<uint64_t> g_resolveNull{ 0 };

std::atomic<uint64_t> g_mlgEnter{ 0 };
std::atomic<uint64_t> g_mlgKeepFrom{ 0 };
std::atomic<uint64_t> g_mlgMoved{ 0 };

std::atomic<uint64_t> g_remapCall{ 0 };
std::atomic<uint64_t> g_remapNonHeap{ 0 };
std::atomic<uint64_t> g_remapNoGhost{ 0 };
std::atomic<uint64_t> g_remapRouteNull{ 0 };
std::atomic<uint64_t> g_remapReceipt{ 0 };
std::atomic<uint64_t> g_remapWait{ 0 };
std::atomic<uint64_t> g_remapWaitTip{ 0 };
std::atomic<uint64_t> g_remapWaitGiveUp{ 0 };

std::atomic<uint64_t> g_fwdEnter{ 0 };
std::atomic<uint64_t> g_fwdOk{ 0 };
std::atomic<uint64_t> g_fwdNull{ 0 };
std::atomic<uint64_t> g_fwdSame{ 0 };

std::atomic<uint64_t> g_sampleLogged{ 0 };
std::atomic<uint64_t> g_heartbeatN{ 0 };

constexpr uint64_t kLogCap = 4096;
constexpr uint64_t kHeartbeat = 1ull << 20;

#define TV_LD(c) static_cast<unsigned long long>((c).load(std::memory_order_relaxed))

void InstallAtexitOnce()
{
    static std::atomic<bool> installed{ false };
    bool expected = false;
    if (installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        return DiagGate::LegacyOrToken("MRT_GCV2_TOVERFAIL", "toverfail");
    }();
    return on;
}

void Report(const char* why)
{
    if (!Enabled()) {
        return;
    }
    // RATE numerator = resolve_keep_from (parse tried, still handed from).
    // RATE denominator = resolve_enter. unmovable_skip is NOT in either.
    std::fprintf(stderr,
                 "[GCV2][toverfail][census] why=%s "
                 "loadgood_fast=%llu slow_enter=%llu "
                 "unmovable_skip=%llu unmovable_skip_fwd=%llu "
                 "resolve_enter=%llu resolve_moved=%llu resolve_keep_from=%llu resolve_null=%llu "
                 "mlg_enter=%llu mlg_moved=%llu mlg_keep_from=%llu "
                 "remap_call=%llu remap_nonghost=%llu remap_route_null=%llu "
                 "remap_receipt=%llu remap_wait=%llu remap_wait_tip=%llu remap_wait_giveup=%llu "
                 "remap_nonheap=%llu "
                 "fwd_enter=%llu fwd_ok=%llu fwd_null=%llu fwd_same=%llu "
                 "samples_logged=%llu\n",
                 why == nullptr ? "?" : why, TV_LD(g_loadGoodFast), TV_LD(g_slowEnter),
                 TV_LD(g_unmovableSkip), TV_LD(g_unmovableSkipFwd), TV_LD(g_resolveEnter),
                 TV_LD(g_resolveMoved), TV_LD(g_resolveKeepFrom), TV_LD(g_resolveNull),
                 TV_LD(g_mlgEnter), TV_LD(g_mlgMoved), TV_LD(g_mlgKeepFrom), TV_LD(g_remapCall),
                 TV_LD(g_remapNoGhost), TV_LD(g_remapRouteNull), TV_LD(g_remapReceipt),
                 TV_LD(g_remapWait), TV_LD(g_remapWaitTip), TV_LD(g_remapWaitGiveUp),
                 TV_LD(g_remapNonHeap), TV_LD(g_fwdEnter), TV_LD(g_fwdOk), TV_LD(g_fwdNull),
                 TV_LD(g_fwdSame), TV_LD(g_sampleLogged));
    std::fflush(stderr);
}

void NoteLoadGoodFast()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    uint64_t n = g_loadGoodFast.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        std::fprintf(stderr, "[GCV2][toverfail] armed\n");
        std::fflush(stderr);
    } else if ((n & (kHeartbeat - 1)) == 0) {
        g_heartbeatN.fetch_add(1, std::memory_order_relaxed);
        Report("heartbeat");
    }
}

void NoteSlowEnter()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_slowEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteUnmovableSkip(BaseObject* oldTarget, unsigned stateCode, unsigned isForwarded)
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_unmovableSkip.fetch_add(1, std::memory_order_relaxed);
    if (isForwarded != 0u) {
        g_unmovableSkipFwd.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_sampleLogged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
        std::fprintf(stderr,
                     "[GCV2][toverfail][unmovable] obj=%p state=%u is_fwd=%u "
                     "(barrier-moment IsUnmovableFromObject short-circuit)\n",
                     static_cast<void*>(oldTarget), stateCode, isForwarded);
        std::fflush(stderr);
    }
}

void NoteResolveEnter()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_resolveEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveOutcome(BaseObject* oldTarget, BaseObject* loadGood, unsigned moved)
{
    if (!Enabled()) {
        return;
    }
    if (loadGood == nullptr) {
        g_resolveNull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (moved != 0u || loadGood != oldTarget) {
        g_resolveMoved.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_resolveKeepFrom.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteMlgEnter()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_mlgEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteMlgKeepFrom()
{
    if (!Enabled()) {
        return;
    }
    g_mlgKeepFrom.fetch_add(1, std::memory_order_relaxed);
}

void NoteMlgMoved()
{
    if (!Enabled()) {
        return;
    }
    g_mlgMoved.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapCall()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_remapCall.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapNonHeap()
{
    if (!Enabled()) {
        return;
    }
    g_remapNonHeap.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapNoGhost()
{
    if (!Enabled()) {
        return;
    }
    g_remapNoGhost.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapRouteNull()
{
    if (!Enabled()) {
        return;
    }
    g_remapRouteNull.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapReceipt()
{
    if (!Enabled()) {
        return;
    }
    g_remapReceipt.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapWait()
{
    if (!Enabled()) {
        return;
    }
    g_remapWait.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapWaitTip()
{
    if (!Enabled()) {
        return;
    }
    g_remapWaitTip.fetch_add(1, std::memory_order_relaxed);
}

void NoteRemapWaitGiveUp()
{
    if (!Enabled()) {
        return;
    }
    g_remapWaitGiveUp.fetch_add(1, std::memory_order_relaxed);
}

void NoteFwdEnter()
{
    if (!Enabled()) {
        return;
    }
    InstallAtexitOnce();
    g_fwdEnter.fetch_add(1, std::memory_order_relaxed);
}

void NoteFwdOk()
{
    if (!Enabled()) {
        return;
    }
    g_fwdOk.fetch_add(1, std::memory_order_relaxed);
}

void NoteFwdNull()
{
    if (!Enabled()) {
        return;
    }
    g_fwdNull.fetch_add(1, std::memory_order_relaxed);
}

void NoteFwdSame()
{
    if (!Enabled()) {
        return;
    }
    g_fwdSame.fetch_add(1, std::memory_order_relaxed);
}

} // namespace ToverFailDiag
} // namespace MapleRuntime
