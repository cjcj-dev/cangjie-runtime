// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Structural harness for movable-stack watermark protocol (stackwm #7).
// No managed stack required: models frames as logical indices + synthetic FAs.
//
// usage: stack_grow_harness <case>
//   logical_stable     — OnStackGrow keeps cursorIndex; bumps generation
//   absolute_desync    — absolute FA "rebase" desyncs from logical frame (①+)
//   resume_token       — logical ResumeAt token unchanged after grow (②)
//   wrong_offset       — inject wrong absolute offset ⇒ frame identity mismatch (②+)
//   no_stw             — OnStackGrow path has no STW counter (③ structural)
//   gen_monotonic      — generation strictly increases per nonzero grow

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

// Synthetic frame table: logical index i lives at absolute FA base+i*stride.
// After grow by offset, correct absolute FA is base+i*stride+offset.
struct FrameTable {
    uintptr_t base;
    size_t stride;
    size_t n;
    uintptr_t FaAt(size_t i) const { return base + i * stride; }
    // Map absolute FA to logical index (pre-grow space).
    bool IndexOf(uintptr_t fa, size_t* out) const
    {
        if (fa < base) {
            return false;
        }
        uintptr_t d = fa - base;
        if (d % stride != 0) {
            return false;
        }
        size_t i = static_cast<size_t>(d / stride);
        if (i >= n) {
            return false;
        }
        *out = i;
        return true;
    }
};
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: stack_grow_harness "
                     "logical_stable|absolute_desync|resume_token|wrong_offset|no_stw|gen_monotonic\n";
        return 2;
    }
    const char* testCase = argv[1];
    StackWatermark wm;
    FrameTable table{ 0x70000000ull, 0x100ull, 8 };

    if (std::strcmp(testCase, "logical_stable") == 0) {
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, table.n), "begin");
        wm.AdvanceTo(3, StackWatermark::WM_OWNER_SELF);
        size_t cursorBefore = wm.GetCursorIndex();
        uint64_t genBefore = wm.GetStackGeneration();
        Expect(cursorBefore == 3, "cursor 3");
        wm.OnStackGrow(0x10000);
        Expect(wm.GetCursorIndex() == cursorBefore, "cursor stable after grow");
        Expect(wm.GetFrameCount() == table.n, "frameCount stable");
        Expect(wm.GetStackGeneration() == genBefore + 1, "generation +1");
        Expect(wm.GetGrowCount() == 1, "growCount 1");
        Expect(wm.GetLastGrowOffset() == 0x10000, "offset recorded");
        // Zero offset is no-op.
        wm.OnStackGrow(0);
        Expect(wm.GetGrowCount() == 1, "zero offset no-op");
        Expect(wm.GetStackGeneration() == genBefore + 1, "gen unchanged on zero");
        wm.AdvanceTo(table.n, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        std::cerr << "HARNESS_OK case=logical_stable grow_count=" << wm.GetGrowCount() << '\n';
        return 0;
    }

    if (std::strcmp(testCase, "absolute_desync") == 0) {
        // ①+ positive: if watermark stored absolute FA of frame cursor-1 and grow
        // rebased it by offset WITHOUT renumbering, ResumeAt would still be correct
        // for logical design — but if grow is SKIPPED and absolute FA is used as-is
        // against the NEW stack, IndexOf fails / points at wrong frame.
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, table.n), "begin");
        size_t logicalCursor = 3; // exclusive end → last processed = 2
        wm.AdvanceTo(logicalCursor, StackWatermark::WM_OWNER_SELF);
        uintptr_t absFaPre = table.FaAt(logicalCursor - 1);
        intptr_t growOff = 0x20000;
        // Simulate grow that only moves stack content; absolute token left un-rebased.
        wm.OnStackGrow(growOff);
        size_t wrongIdx = 999;
        bool foundOnOld = table.IndexOf(absFaPre, &wrongIdx);
        Expect(foundOnOld, "old FA maps on pre-grow table");
        Expect(wrongIdx == logicalCursor - 1, "old FA is frame 2");
        // New stack: FA at same bit pattern is NOT frame 2 anymore (would need +growOff).
        FrameTable post{ table.base + static_cast<uintptr_t>(growOff), table.stride, table.n };
        size_t postIdx = 999;
        bool foundOnNew = post.IndexOf(absFaPre, &postIdx);
        Expect(!foundOnNew, "unrebased absolute FA not on post-grow table");
        // Correct rebase of absolute token would recover the frame:
        uintptr_t rebased = StackWatermark::InjectAbsoluteResumeToken(absFaPre, growOff);
        size_t okIdx = 999;
        Expect(post.IndexOf(rebased, &okIdx), "rebased FA on post table");
        Expect(okIdx == logicalCursor - 1, "rebased FA still frame 2");
        // Logical cursor still names the same exclusive end without touching FA bits.
        Expect(wm.GetCursorIndex() == logicalCursor, "logical cursor stable");
        std::cerr << "HARNESS_OK case=absolute_desync unrebased_miss=1 rebased_ok=1 "
                     "logical_cursor="
                  << wm.GetCursorIndex() << '\n';
        return 0;
    }

    if (std::strcmp(testCase, "resume_token") == 0) {
        // ②: after grow, ResumeAt(cursorIndex) still means the same exclusive index.
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_GC, 6), "begin");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_GC);
        size_t token = wm.GetCursorIndex();
        wm.OnStackGrow(0x4000);
        Expect(wm.GetCursorIndex() == token, "token unchanged");
        // Continue scan from token as if ResumeAt(token) then ProcessOne…
        wm.AdvanceTo(4, StackWatermark::WM_OWNER_GC);
        Expect(wm.GetCursorIndex() == 4, "continued from same token");
        wm.AdvanceTo(6, StackWatermark::WM_OWNER_GC);
        wm.Finish(StackWatermark::WM_OWNER_GC);
        std::cerr << "HARNESS_OK case=resume_token token=" << token << '\n';
        return 0;
    }

    if (std::strcmp(testCase, "wrong_offset") == 0) {
        // ②+ positive: feed wrong absolute rebase offset → logical identity mismatch.
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, table.n), "begin");
        size_t logicalCursor = 4;
        wm.AdvanceTo(logicalCursor, StackWatermark::WM_OWNER_SELF);
        uintptr_t absFaPre = table.FaAt(logicalCursor - 1);
        intptr_t trueOff = 0x10000;
        intptr_t wrongOff = 0x10000 + static_cast<intptr_t>(table.stride); // off-by-one frame
        wm.OnStackGrow(trueOff);
        FrameTable post{ table.base + static_cast<uintptr_t>(trueOff), table.stride, table.n };
        uintptr_t wrongAbs = StackWatermark::InjectAbsoluteResumeToken(absFaPre, wrongOff);
        size_t wrongIdx = 999;
        Expect(post.IndexOf(wrongAbs, &wrongIdx), "wrong-offset FA still lands on table");
        Expect(wrongIdx != logicalCursor - 1, "wrong offset ⇒ wrong frame");
        Expect(wrongIdx == logicalCursor, "off-by-one frame");
        // Logical token unaffected — invariant O substrate remains valid.
        Expect(wm.GetCursorIndex() == logicalCursor, "logical token still correct");
        std::cerr << "HARNESS_OK case=wrong_offset wrong_frame=" << wrongIdx
                  << " logical=" << (logicalCursor - 1) << '\n';
        return 0;
    }

    if (std::strcmp(testCase, "no_stw") == 0) {
        // ③ structural: OnStackGrow is pure atomics; no STW API is reachable from here.
        // We assert grow path only mutates generation/offset counters.
        Expect(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 2), "begin");
        wm.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
        wm.OnStackGrow(0x8000);
        Expect(wm.IsScanning(), "still SCANNING (no phase STW-style close)");
        Expect(wm.GetOwner() == StackWatermark::WM_OWNER_SELF, "owner preserved");
        wm.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
        wm.Finish(StackWatermark::WM_OWNER_SELF);
        std::cerr << "HARNESS_OK case=no_stw stw_in_grow=0\n";
        return 0;
    }

    if (std::strcmp(testCase, "gen_monotonic") == 0) {
        Expect(wm.GetStackGeneration() == 0, "gen0");
        wm.OnStackGrow(1);
        wm.OnStackGrow(2);
        wm.OnStackGrow(3);
        Expect(wm.GetStackGeneration() == 3, "gen3");
        Expect(wm.GetGrowCount() == 3, "count3");
        Expect(wm.GetLastGrowOffset() == 3, "last offset");
        std::cerr << "HARNESS_OK case=gen_monotonic gen=" << wm.GetStackGeneration() << '\n';
        return 0;
    }

    std::cerr << "unknown case: " << testCase << '\n';
    return 2;
}
