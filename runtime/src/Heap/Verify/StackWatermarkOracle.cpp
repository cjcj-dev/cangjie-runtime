// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/StackWatermarkOracle.h"

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

// Drive a full begin→process→finish on the mutator's watermark, then resume.
// midIndex is the watermark position published after the first half of frames.
void DriveAndCompare(const UnwindContext& topFrame, Mutator& mutator, size_t midIndex, bool badResume)
{
    StackFrameCursor fullCursor(topFrame);
    size_t n = fullCursor.FrameCount();
    if (midIndex > n) {
        midIndex = n;
    }

    StackWatermark& wm = mutator.GetStackWatermark();
    wm.Reset();
    constexpr uint64_t kEpoch = 1;
    bool began = wm.TryBegin(kEpoch, StackWatermark::WM_OWNER_GC, n);
    if (!began) {
        LOG(RTLOG_ERROR, "[GCV2][stack-watermark-oracle] TryBegin failed mutator=%p frames=%zu", &mutator, n);
        g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Full-drain multiset (oracle ground truth for remaining roots after resume).
    std::vector<RootKey> fullKeys;
    {
        StackFrameCursor c(topFrame);
        RootVisitor v = MakeCollector(fullKeys);
        c.ProcessAll(v, mutator);
    }

    // Process first midIndex frames via ProcessOne, AdvanceTo, then Finish after rest
    // OR: AdvanceTo(mid), Finish only after draining via ResumeAt.
    std::vector<RootKey> firstHalf;
    {
        StackFrameCursor c(topFrame);
        RootVisitor v = MakeCollector(firstHalf);
        while (c.Cursor() < midIndex && !c.Done()) {
            (void)c.ProcessOne(v, mutator);
        }
        wm.AdvanceTo(c.Cursor(), StackWatermark::WM_OWNER_GC);
    }

    // Resume arm: ResumeAt(wm.cursor) then drain remainder.
    size_t resumeAt = wm.GetCursorIndex();
    std::vector<RootKey> resumeKeys = firstHalf;
    if (badResume) {
        // Positive control: discard the first-half contribution and resume one past the
        // watermark. Roots that lived only in [0, mid) are lost even when the sole root
        // sits in the first half (the common managed-frame shape for hello.cj).
        resumeKeys.clear();
        if (resumeAt < n) {
            resumeAt = resumeAt + 1;
        } else if (n > 0) {
            resumeAt = 0;
        }
    }
    {
        StackFrameCursor c(topFrame);
        if (!c.ResumeAt(resumeAt, mutator)) {
            LOG(RTLOG_ERROR,
                "[GCV2][stack-watermark-oracle] ResumeAt failed resume=%zu frames=%zu bad=%d",
                resumeAt, n, badResume ? 1 : 0);
            g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
            wm.Reset();
            return;
        }
        RootVisitor v = MakeCollector(resumeKeys);
        c.ProcessAll(v, mutator);
        wm.AdvanceTo(c.Cursor(), StackWatermark::WM_OWNER_GC);
        wm.Finish(StackWatermark::WM_OWNER_GC);
    }

    std::sort(fullKeys.begin(), fullKeys.end());
    std::sort(resumeKeys.begin(), resumeKeys.end());
    bool match = fullKeys == resumeKeys;
    g_exerciseCount.fetch_add(1, std::memory_order_relaxed);
    if (match) {
        g_matchCount.fetch_add(1, std::memory_order_relaxed);
        // ERROR (not DLOG/INFO): verify arm must be observable at default log level.
        LOG(RTLOG_ERROR,
            "[GCV2][stack-watermark-oracle] MATCH mutator=%p roots=%zu frames=%zu mid=%zu bad=%d "
            "env=MRT_GCV2_STACK_WATERMARK_VERIFY=1",
            &mutator, fullKeys.size(), n, midIndex, badResume ? 1 : 0);
        if (badResume && !fullKeys.empty() && n > 0) {
            // Wrong resume should have mismatched when there were roots/frames to lose.
            // If still match, positive control is silent — fail closed.
            LOG(RTLOG_ERROR,
                "[GCV2][stack-watermark-oracle] POSITIVE_CONTROL_SILENT bad_resume still MATCH "
                "roots=%zu frames=%zu mid=%zu",
                fullKeys.size(), n, midIndex);
            g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
            g_matchCount.fetch_sub(1, std::memory_order_relaxed);
        }
        return;
    }

    g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[GCV2][stack-watermark-oracle] MISMATCH mutator=%p full_roots=%zu resume_roots=%zu "
        "frames=%zu mid=%zu bad=%d env=MRT_GCV2_STACK_WATERMARK_VERIFY=1",
        &mutator, fullKeys.size(), resumeKeys.size(), n, midIndex, badResume ? 1 : 0);
    if (StackWatermarkOracle::FatalEnabled() && !badResume) {
        LOG(RTLOG_FATAL, "[GCV2][stack-watermark-oracle] fatal multiset mismatch");
    }
}
} // namespace

bool StackWatermarkOracle::Enabled()
{
    // Verify flag enables both StackWatermark CHECKs and this STW exercise.
    return StackWatermark::VerifyEnabled();
}

bool StackWatermarkOracle::FatalEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_STACK_WATERMARK_FATAL */;
    return on;
}

const char* StackWatermarkOracle::InjectName()
{
    static const char* name = nullptr /* pinned:MRT_GCV2_STACK_WATERMARK_INJECT */;
    return name;
}

void StackWatermarkOracle::Exercise(const UnwindContext& topFrame, Mutator& mutator)
{
    if (!Enabled()) {
        return;
    }
    // Breadcrumb: prove the product VisitStackRoots path reached us (loglevel-independent).
    LOG(RTLOG_ERROR,
        "[GCV2][stack-watermark-oracle] ENTER mutator=%p world_stopped=%d inject=%s "
        "env=MRT_GCV2_STACK_WATERMARK_VERIFY=1",
        &mutator, MutatorManager::Instance().WorldStopped() ? 1 : 0, InjectName());
    if (!MutatorManager::Instance().WorldStopped()) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-watermark-oracle] refused: world not stopped "
            "env=MRT_GCV2_STACK_WATERMARK_VERIFY=1");
        return;
    }

    const char* inject = InjectName();
    StackWatermark& wm = mutator.GetStackWatermark();

    // ① Illegal phase transition: begin while SCANNING.
    if (std::strcmp(inject, "illegal_transition") == 0) {
        wm.InjectIllegalPhaseBack();
        // TryBegin must CHECK-fail under verify.
        (void)wm.TryBegin(2, StackWatermark::WM_OWNER_GC, 1);
        // Unreachable if CHECK fired.
        LOG(RTLOG_ERROR, "[GCV2][stack-watermark-oracle] INJECT_SILENT illegal_transition");
        return;
    }

    // ② Dual owner: second claim while first holds SCANNING.
    if (std::strcmp(inject, "dual_owner") == 0) {
        wm.InjectDualOwner(StackWatermark::WM_OWNER_GC);
        (void)wm.TryBegin(1, StackWatermark::WM_OWNER_GC, 1);
        LOG(RTLOG_ERROR, "[GCV2][stack-watermark-oracle] INJECT_SILENT dual_owner");
        return;
    }

    // ③ Exit while SCANNING (lifecycle).
    if (std::strcmp(inject, "exit_scanning") == 0) {
        wm.Reset();
        (void)wm.TryBegin(1, StackWatermark::WM_OWNER_GC, 4);
        wm.OnExit(); // must CHECK under verify
        LOG(RTLOG_ERROR, "[GCV2][stack-watermark-oracle] INJECT_SILENT exit_scanning");
        return;
    }

    // Happy path or bad_resume positive control for invariant O.
    bool badResume = std::strcmp(inject, "bad_resume") == 0;
    StackFrameCursor probe(topFrame);
    size_t n = probe.FrameCount();
    size_t mid = n / 2;
    DriveAndCompare(topFrame, mutator, mid, badResume);
}

size_t StackWatermarkOracle::ExerciseCount() { return g_exerciseCount.load(std::memory_order_relaxed); }
size_t StackWatermarkOracle::MatchCount() { return g_matchCount.load(std::memory_order_relaxed); }
size_t StackWatermarkOracle::MismatchCount() { return g_mismatchCount.load(std::memory_order_relaxed); }
void StackWatermarkOracle::ResetStats()
{
    g_exerciseCount.store(0, std::memory_order_relaxed);
    g_matchCount.store(0, std::memory_order_relaxed);
    g_mismatchCount.store(0, std::memory_order_relaxed);
}

} // namespace MapleRuntime
