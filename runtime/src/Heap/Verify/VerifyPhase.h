// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_VERIFY_PHASE_H
#define MRT_VERIFY_PHASE_H

namespace MapleRuntime {

// The five ZGC verification faces. All verifier admission goes through this table.
enum class VerifyFace { Roots = 0, Objects, Marking, Remembered, Oops };

const char* VerifyFaceName(VerifyFace face);
bool VerifyFaceEnabled(VerifyFace face);
bool VerifyPhaseEnter(VerifyFace face, const char* phase);

} // namespace MapleRuntime

#endif // MRT_VERIFY_PHASE_H
