// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_WATERMARK_ORACLE_H
#define MRT_STACK_WATERMARK_ORACLE_H

#include <cstddef>
#include <cstdint>

#include "Common/StackType.h"

namespace MapleRuntime {

class Mutator;

// STW-only stack-watermark structural oracle (stackwm #1).
// Gates (default off):
//   MRT_GCV2_STACK_WATERMARK_VERIFY=1   — enable state-machine CHECKs + STW exercise
//   MRT_GCV2_STACK_WATERMARK_INJECT=<name> — positive-control injectors (see .cpp)
//   MRT_GCV2_STACK_WATERMARK_FATAL=1    — abort on unexpected mismatch (not on inject arms)
//
// Product path: no concurrent stack scan; only exercises state + cursor ResumeAt under STW.
class StackWatermarkOracle {
public:
    static bool Enabled();
    static bool FatalEnabled();
    static const char* InjectName();

    // Under WorldStopped + MutatorLock: begin/advance/finish watermark against a real
    // StackFrameCursor, then ResumeAt(watermark) and drain remaining roots; compare to a
    // full ProcessAll multiset (invariant O for the resume token).
    static void Exercise(const UnwindContext& topFrame, Mutator& mutator);

    static size_t ExerciseCount();
    static size_t MatchCount();
    static size_t MismatchCount();
    static void ResetStats();
};

} // namespace MapleRuntime

#endif // MRT_STACK_WATERMARK_ORACLE_H
