// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Structural harness for StackExposureHook (stackwm #5).
// No managed stack: models frames as indices on StackWatermark.
//
// usage: stack_exposure_harness <case>
//   fire_on_cross     — return to unscanned frame fires hook + advances (① happy)
//   observe_cross     — cross observed when process disabled (① positive)
//   empty_process     — hook fires, watermark does not advance (② positive)
//   no_stw            — hook path does not call StopTheWorld (④)
//   already_scanned   — expose already-processed frame → no fire
//   done_epoch        — DONE phase → no fire

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "UnwindStack/StackExposureHook.h"
#include "UnwindStack/StackWatermark.h"

using MapleRuntime::StackExposureHook;
using MapleRuntime::StackWatermark;

namespace {
void Expect(bool cond, const char* msg)
{
    if (!cond) {
        std::cerr << "HARNESS_FAIL " << msg << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: stack_exposure_harness "
                     "fire_on_cross|observe_cross|empty_process|no_stw|already_scanned|done_epoch\n";
        return 2;
    }
    const char* testCase = argv[1];
    StackWatermark wm;
    StackExposureHook::ResetStats();

    if (std::strcmp(testCase, "fire_on_cross") == 0) {
        // 4 frames; process [0,2); expose frame 2.
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 4), "begin");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
        Expect(StackExposureHook::NeedsProcess(wm, 2), "needs process for frame 2");
        bool fired = StackExposureHook::OnBeforeUnwind(wm, 2, StackExposureHook::AdvanceOnlyProcess);
        Expect(fired, "hook fired");
        Expect(StackExposureHook::FireCount() == 1, "fire count 1");
        Expect(StackExposureHook::AdvanceCount() == 1, "advance count 1");
        Expect(wm.GetCursorIndex() >= 3, "cursor covers frame 2");
        Expect(!StackExposureHook::NeedsProcess(wm, 2), "frame 2 now safe");
        Expect(StackExposureHook::StopTheWorldCallsInHook() == 0, "no STW");
        std::cerr << "HARNESS_OK case=fire_on_cross fire=1 advance=1 cursor=" << wm.GetCursorIndex()
                  << " stw=0\n";
        return 0;
    }

    if (std::strcmp(testCase, "observe_cross") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 4), "begin");
        wm.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
        size_t before = wm.GetCursorIndex();
        bool observed = StackExposureHook::ObserveCrossWithoutProcess(wm, 3);
        Expect(observed, "cross observed");
        Expect(StackExposureHook::CrossWithoutProcessCount() == 1, "cross count 1");
        Expect(wm.GetCursorIndex() == before, "cursor unchanged");
        Expect(StackExposureHook::FireCount() == 0, "no process fire");
        std::cerr << "HARNESS_OK case=observe_cross cross=1 cursor_unchanged=" << before << "\n";
        return 0;
    }

    if (std::strcmp(testCase, "empty_process") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 4), "begin");
        wm.AdvanceTo(0, StackWatermark::WM_OWNER_SELF);
        size_t before = wm.GetCursorIndex();
        bool fired = StackExposureHook::OnBeforeUnwind(wm, 1, StackExposureHook::NoopProcess);
        Expect(fired, "hook fired");
        Expect(StackExposureHook::FireCount() == 1, "fire 1");
        Expect(StackExposureHook::AdvanceCount() == 0, "no advance");
        Expect(wm.GetCursorIndex() == before, "cursor stuck");
        Expect(StackExposureHook::NeedsProcess(wm, 1), "still needs process");
        std::cerr << "HARNESS_OK case=empty_process fire=1 advance=0 cursor=" << before << "\n";
        return 0;
    }

    if (std::strcmp(testCase, "no_stw") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 3), "begin");
        // Nested STW probe: only counts if NoteStopTheWorldFromHook during hook depth.
        bool fired = StackExposureHook::OnBeforeUnwind(wm, 0, StackExposureHook::AdvanceOnlyProcess);
        Expect(fired, "fired");
        Expect(StackExposureHook::StopTheWorldCallsInHook() == 0, "stw in hook == 0");
        // Outside hook, Note should not count.
        StackExposureHook::NoteStopTheWorldFromHook();
        Expect(StackExposureHook::StopTheWorldCallsInHook() == 0, "outside hook no count");
        std::cerr << "HARNESS_OK case=no_stw stw_in_hook=0\n";
        return 0;
    }

    if (std::strcmp(testCase, "already_scanned") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 4), "begin");
        wm.AdvanceTo(3, StackWatermark::WM_OWNER_SELF);
        Expect(!StackExposureHook::NeedsProcess(wm, 1), "frame 1 already covered");
        bool fired = StackExposureHook::OnBeforeUnwind(wm, 1, StackExposureHook::AdvanceOnlyProcess);
        Expect(!fired, "no fire");
        Expect(StackExposureHook::FireCount() == 0, "fire 0");
        std::cerr << "HARNESS_OK case=already_scanned fire=0\n";
        return 0;
    }

    if (std::strcmp(testCase, "done_epoch") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 2), "begin");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        Expect(wm.IsDone(), "DONE");
        Expect(!StackExposureHook::NeedsProcess(wm, 0), "DONE no need");
        bool fired = StackExposureHook::OnBeforeUnwind(wm, 0, StackExposureHook::AdvanceOnlyProcess);
        Expect(!fired, "no fire on DONE");
        std::cerr << "HARNESS_OK case=done_epoch fire=0\n";
        return 0;
    }

    std::cerr << "unknown case: " << testCase << '\n';
    return 2;
}
