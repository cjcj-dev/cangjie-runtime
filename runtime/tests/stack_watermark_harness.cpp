// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Structural harness for StackWatermark state machine (stackwm #1).
// Does not need a managed stack: exercises pure state transitions + lifecycle.
// Positive controls must abort (rc=134) when MRT_GCV2_STACK_WATERMARK_VERIFY=1.
//
// usage: stack_watermark_harness <case>
//   happy            — NOT_STARTED → SCANNING → DONE
//   illegal_transition — begin while SCANNING (must CHECK)
//   dual_owner       — second claim (must CHECK)
//   exit_scanning    — OnExit while SCANNING (must CHECK)
//   park_ok          — OnPark in each phase does not regress
//   create_closed    — OnCreate from DONE is closed

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "UnwindStack/StackWatermark.h"

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
        std::cerr << "usage: stack_watermark_harness "
                     "happy|illegal_transition|dual_owner|exit_scanning|park_ok|create_closed\n";
        return 2;
    }
    const char* testCase = argv[1];
    StackWatermark wm;

    if (std::strcmp(testCase, "happy") == 0) {
        Expect(wm.IsNotStarted(), "fresh NOT_STARTED");
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 4), "begin");
        Expect(wm.IsScanning(), "SCANNING after begin");
        Expect(wm.GetOwner() == StackWatermark::WM_OWNER_SELF, "owner SELF");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
        Expect(wm.GetCursorIndex() == 2, "cursor 2");
        wm.AdvanceTo(4, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        Expect(wm.IsDone(), "DONE");
        Expect(wm.GetOwner() == StackWatermark::WM_OWNER_NONE, "owner cleared");
        // New epoch after DONE is legal.
        Expect(wm.TryBegin(2, StackWatermark::WM_OWNER_GC, 1), "begin epoch 2");
        wm.AdvanceTo(1, StackWatermark::WM_OWNER_GC);
        wm.Finish(StackWatermark::WM_OWNER_GC);
        std::cerr << "HARNESS_OK case=happy\n";
        return 0;
    }

    if (std::strcmp(testCase, "illegal_transition") == 0) {
        wm.InjectIllegalPhaseBack();
        std::cerr << "HARNESS_INJECT illegal_transition calling TryBegin\n";
        (void)wm.TryBegin(2, StackWatermark::WM_OWNER_GC, 1);
        std::cerr << "HARNESS_SILENT illegal_transition\n";
        return 1;
    }

    if (std::strcmp(testCase, "dual_owner") == 0) {
        wm.InjectDualOwner(StackWatermark::WM_OWNER_GC);
        std::cerr << "HARNESS_INJECT dual_owner calling TryBegin\n";
        (void)wm.TryBegin(1, StackWatermark::WM_OWNER_GC, 1);
        std::cerr << "HARNESS_SILENT dual_owner\n";
        return 1;
    }

    if (std::strcmp(testCase, "exit_scanning") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_GC, 4), "begin");
        std::cerr << "HARNESS_INJECT exit_scanning calling OnExit\n";
        wm.OnExit();
        std::cerr << "HARNESS_SILENT exit_scanning\n";
        return 1;
    }

    if (std::strcmp(testCase, "park_ok") == 0) {
        wm.OnPark(); // NOT_STARTED
        Expect(wm.IsNotStarted(), "park from NOT_STARTED");
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 2), "begin");
        wm.OnPark(); // SCANNING — must not regress
        Expect(wm.IsScanning(), "park keeps SCANNING");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        wm.OnPark(); // DONE
        Expect(wm.IsDone(), "park keeps DONE");
        std::cerr << "HARNESS_OK case=park_ok\n";
        return 0;
    }

    if (std::strcmp(testCase, "create_closed") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 1), "begin");
        wm.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        wm.OnCreate(); // DONE → reset to NOT_STARTED
        Expect(wm.IsNotStarted(), "OnCreate closes to NOT_STARTED");
        Expect(wm.GetEpoch() == 0, "epoch cleared");
        std::cerr << "HARNESS_OK case=create_closed\n";
        return 0;
    }

    std::cerr << "unknown case: " << testCase << '\n';
    return 2;
}
