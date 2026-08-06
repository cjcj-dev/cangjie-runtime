// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B4_BENIGN_PROBE_H
#define MRT_HEAP_B4_BENIGN_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Consume-side probe: fate of interior pointers once they enter young workStack.
// Gate (default off): MRT_GCV2_B4BENIGN=1
// Pairs with MRT_GCV2_INTERIOR_SRC=1 (base-first classifier) for push classification.
// Does NOT touch write paths (b4persist territory). Does NOT relax validity checks.
class B4BenignProbe {
public:
    static bool Enabled();

    // Call when TraceYoungClosure pops an object and is about to treat it as base
    // (IsValidObject / MarkObject / ForEachRefField).
    static void NoteConsumeAsBase(void* object, const char* site);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B4_BENIGN_PROBE_H
