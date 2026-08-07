// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_EXPOSURE_ORACLE_H
#define MRT_STACK_EXPOSURE_ORACLE_H

#include <cstddef>

#include "Common/StackType.h"

namespace MapleRuntime {

class Mutator;

// STW-only frame-exposure oracle (stackwm #5).
// Gates (default off):
//   MRT_GCV2_STACK_EXPOSURE_VERIFY=1  — enable exercise + structural CHECKs
//   MRT_GCV2_STACK_EXPOSURE_INJECT=<name>
//     empty_process  — hook fires but process is no-op (watermark does not advance)
//     observe_cross  — observe return-to-unscanned without process (positive for ①)
//
// Under WorldStopped: partial-scan watermark, then OnBeforeUnwind with real
// StackFrameCursor::ProcessOne as processFn; remaining multiset must equal
// full ProcessAll (invariant O via same RootKey as StackFrameOracle).
class StackExposureOracle {
public:
    static bool Enabled();
    static bool FatalEnabled();
    static const char* InjectName();

    static void Exercise(const UnwindContext& topFrame, Mutator& mutator);

    static size_t ExerciseCount();
    static size_t MatchCount();
    static size_t MismatchCount();
    static void ResetStats();
};

} // namespace MapleRuntime

#endif // MRT_STACK_EXPOSURE_ORACLE_H
