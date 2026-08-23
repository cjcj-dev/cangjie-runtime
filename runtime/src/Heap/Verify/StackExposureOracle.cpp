// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/StackExposureOracle.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "UnwindStack/StackExposureHook.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackWatermark.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_exerciseCount{ 0 };
std::atomic<size_t> g_matchCount{ 0 };
std::atomic<size_t> g_mismatchCount{ 0 };

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* EnvStr(const char* name)
{
    const char* v = std::getenv(name);
    return v == nullptr ? "" : v;
}

using RootKey = std::pair<uintptr_t, uintptr_t>;

RootVisitor MakeCollector(std::vector<RootKey>& keys)
{
    return [&keys](ObjectRef& root) {
        keys.emplace_back(reinterpret_cast<uintptr_t>(&root), raw(root.LoadPlain()));
    };
}

// ProcessFn that drives StackFrameCursor::ProcessOne until needUpTo, AdvanceTo.
StackExposureHook::ProcessFn MakeCursorProcess(StackFrameCursor& cursor, Mutator& mutator,
                                               std::vector<RootKey>& keys)
{
    return [&cursor, &mutator, &keys](StackWatermark& wm, size_t needUpTo) -> size_t {
        RootVisitor v = MakeCollector(keys);
        while (cursor.Cursor() < needUpTo && !cursor.Done()) {
            (void)cursor.ProcessOne(v, mutator);
        }
        size_t idx = cursor.Cursor();
        StackWatermark::Owner o = wm.GetOwner();
        if (o != StackWatermark::WM_OWNER_NONE) {
            wm.AdvanceTo(idx, o);
        }
        return idx;
    };
}

void DriveExposureAndCompare(const UnwindContext& topFrame, Mutator& mutator, bool emptyProcess,
                             bool observeOnly)
{
    StackFrameCursor probe(topFrame);
    size_t n = probe.FrameCount();
    if (n == 0) {
        LOG(RTLOG_ERROR, "[GCV2][stack-exposure-oracle] empty stack mutator=%p", &mutator);
        return;
    }

    // Partial scan: leave at least one unprocessed frame when possible.
    size_t mid = n > 1 ? n / 2 : 0;
    size_t exposeIdx = mid; // first unprocessed frame after mid

    StackWatermark& wm = mutator.GetStackWatermark();
    wm.Reset();
    StackExposureHook::ResetStats();
    constexpr uint64_t kEpoch = 1;
    if (!wm.TryBegin(kEpoch, StackWatermark::WM_OWNER_GC, n)) {
        LOG(RTLOG_ERROR, "[GCV2][stack-exposure-oracle] TryBegin failed frames=%zu", n);
        g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Full multiset ground truth.
    std::vector<RootKey> fullKeys;
    {
        StackFrameCursor c(topFrame);
        RootVisitor v = MakeCollector(fullKeys);
        c.ProcessAll(v, mutator);
    }

    // Process [0, mid) offline (simulates GC partial scan before mutator returns).
    std::vector<RootKey> collected;
    StackFrameCursor work(topFrame);
    {
        RootVisitor v = MakeCollector(collected);
        while (work.Cursor() < mid && !work.Done()) {
            (void)work.ProcessOne(v, mutator);
        }
        wm.AdvanceTo(work.Cursor(), StackWatermark::WM_OWNER_GC);
    }

    // ① positive: observe cross without process.
    if (observeOnly) {
        bool crossed = StackExposureHook::ObserveCrossWithoutProcess(wm, exposeIdx);
        LOG(RTLOG_ERROR,
            "[GCV2][stack-exposure-oracle] OBSERVE_CROSS crossed=%d frame=%zu cursor=%zu "
            "cross_count=%zu env=MRT_GCV2_STACK_EXPOSURE_VERIFY=1",
            crossed ? 1 : 0, exposeIdx, wm.GetCursorIndex(),
            StackExposureHook::CrossWithoutProcessCount());
        if (!crossed && exposeIdx >= wm.GetCursorIndex() && wm.IsScanning()) {
            g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
            LOG(RTLOG_ERROR, "[GCV2][stack-exposure-oracle] OBSERVE_CROSS silent (expected fire)");
        }
        wm.Reset();
        g_exerciseCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Hook: return into exposeIdx.
    StackExposureHook::ProcessFn processFn =
        emptyProcess ? StackExposureHook::ProcessFn(StackExposureHook::NoopProcess)
                     : MakeCursorProcess(work, mutator, collected);

    size_t fireBefore = StackExposureHook::FireCount();
    size_t advBefore = StackExposureHook::AdvanceCount();
    size_t cursorBefore = wm.GetCursorIndex();
    bool fired = StackExposureHook::OnBeforeUnwind(wm, exposeIdx, processFn);
    size_t fireAfter = StackExposureHook::FireCount();
    size_t advAfter = StackExposureHook::AdvanceCount();
    size_t cursorAfter = wm.GetCursorIndex();
    size_t stw = StackExposureHook::StopTheWorldCallsInHook();

    LOG(RTLOG_ERROR,
        "[GCV2][stack-exposure-oracle] HOOK fired=%d empty=%d frame=%zu cursor %zu->%zu "
        "fire=%zu->%zu adv=%zu->%zu stw_in_hook=%zu env=MRT_GCV2_STACK_EXPOSURE_VERIFY=1",
        fired ? 1 : 0, emptyProcess ? 1 : 0, exposeIdx, cursorBefore, cursorAfter, fireBefore, fireAfter,
        advBefore, advAfter, stw);

    if (emptyProcess) {
        // ② positive: fire but no advance.
        if (!(fired && fireAfter > fireBefore && cursorAfter == cursorBefore && advAfter == advBefore)) {
            LOG(RTLOG_ERROR,
                "[GCV2][stack-exposure-oracle] EMPTY_PROCESS positive control failed "
                "fired=%d cursor %zu->%zu adv %zu->%zu",
                fired ? 1 : 0, cursorBefore, cursorAfter, advBefore, advAfter);
            g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_matchCount.fetch_add(1, std::memory_order_relaxed);
            LOG(RTLOG_ERROR,
                "[GCV2][stack-exposure-oracle] EMPTY_PROCESS_OK fire+no_advance frame=%zu", exposeIdx);
        }
        wm.Reset();
        g_exerciseCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Happy path: process advanced past exposeIdx; drain rest; multiset == full.
    if (!fired || cursorAfter <= exposeIdx) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-exposure-oracle] happy path did not cover frame fired=%d cursor=%zu frame=%zu",
            fired ? 1 : 0, cursorAfter, exposeIdx);
        g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
        wm.Reset();
        g_exerciseCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Drain remaining via cursor (work already at cursorAfter).
    {
        RootVisitor v = MakeCollector(collected);
        work.ProcessAll(v, mutator);
        wm.AdvanceTo(work.Cursor(), StackWatermark::WM_OWNER_GC);
        wm.Finish(StackWatermark::WM_OWNER_GC);
    }

    std::sort(fullKeys.begin(), fullKeys.end());
    std::sort(collected.begin(), collected.end());
    bool match = fullKeys == collected;
    g_exerciseCount.fetch_add(1, std::memory_order_relaxed);
    if (match && stw == 0) {
        g_matchCount.fetch_add(1, std::memory_order_relaxed);
        LOG(RTLOG_ERROR,
            "[GCV2][stack-exposure-oracle] MATCH mutator=%p roots=%zu frames=%zu mid=%zu expose=%zu "
            "stw_in_hook=%zu env=MRT_GCV2_STACK_EXPOSURE_VERIFY=1",
            &mutator, fullKeys.size(), n, mid, exposeIdx, stw);
        return;
    }

    g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[GCV2][stack-exposure-oracle] MISMATCH mutator=%p full=%zu got=%zu frames=%zu "
        "mid=%zu expose=%zu stw=%zu match=%d",
        &mutator, fullKeys.size(), collected.size(), n, mid, exposeIdx, stw, match ? 1 : 0);
    if (StackExposureOracle::FatalEnabled() && !emptyProcess && !observeOnly) {
        LOG(RTLOG_FATAL, "[GCV2][stack-exposure-oracle] fatal multiset mismatch or STW in hook");
    }
}
} // namespace

bool StackExposureOracle::Enabled()
{
    return StackExposureHook::VerifyEnabled();
}

bool StackExposureOracle::FatalEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_STACK_EXPOSURE_FATAL */;
    return on;
}

const char* StackExposureOracle::InjectName()
{
    static const char* name = nullptr /* pinned:MRT_GCV2_STACK_EXPOSURE_INJECT */;
    return name;
}

void StackExposureOracle::Exercise(const UnwindContext& topFrame, Mutator& mutator)
{
    if (!Enabled()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][stack-exposure-oracle] ENTER mutator=%p world_stopped=%d inject=%s "
        "env=MRT_GCV2_STACK_EXPOSURE_VERIFY=1",
        &mutator, MutatorManager::Instance().WorldStopped() ? 1 : 0, InjectName());
    if (!MutatorManager::Instance().WorldStopped()) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-exposure-oracle] refused: world not stopped "
            "env=MRT_GCV2_STACK_EXPOSURE_VERIFY=1");
        return;
    }

    const char* inject = InjectName();
    bool emptyProcess = std::strcmp(inject, "empty_process") == 0;
    bool observeOnly = std::strcmp(inject, "observe_cross") == 0;
    DriveExposureAndCompare(topFrame, mutator, emptyProcess, observeOnly);
}

size_t StackExposureOracle::ExerciseCount() { return g_exerciseCount.load(std::memory_order_relaxed); }
size_t StackExposureOracle::MatchCount() { return g_matchCount.load(std::memory_order_relaxed); }
size_t StackExposureOracle::MismatchCount() { return g_mismatchCount.load(std::memory_order_relaxed); }
void StackExposureOracle::ResetStats()
{
    g_exerciseCount.store(0, std::memory_order_relaxed);
    g_matchCount.store(0, std::memory_order_relaxed);
    g_mismatchCount.store(0, std::memory_order_relaxed);
}

} // namespace MapleRuntime
