// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_FRAME_ORACLE_H
#define MRT_STACK_FRAME_ORACLE_H

#include <cstddef>
#include <cstdint>

#include "Common/StackType.h"

namespace MapleRuntime {

class Mutator;

// STW-only dual-run oracle: legacy full-stack VisitStackRoots vs StackFrameCursor.
// Gate (default off): MRT_GCV2_STACK_FRAME_ORACLE=1
// Positive control:   MRT_GCV2_STACK_FRAME_ORACLE_SKIP=1  (cursor skips first MANAGED frame)
// Optional abort:     MRT_GCV2_STACK_FRAME_ORACLE_FATAL=1
//
// Invariant O: under WorldStopped, the multiset of
//   (frameFA, frameIP, kind, id, object)
// from both arms is identical. Product path keeps the legacy visitor.
class StackFrameOracle {
public:
    static bool Enabled();
    static bool SkipFirstManagedEnabled();
    static bool FatalEnabled();

    // Compare legacy vs cursor for one mutator stack. Call only under WorldStopped + MutatorLock.
    static void CompareWithLegacy(const UnwindContext& topFrame, Mutator& mutator);

    static size_t CompareCount();
    static size_t MismatchCount();
    static size_t MatchCount();
    static void ResetStats();
};

} // namespace MapleRuntime

#endif // MRT_STACK_FRAME_ORACLE_H
