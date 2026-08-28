// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/VerifyPhase.h"

#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace {
bool ValueIsOne(const char* value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}
} // namespace

const char* VerifyFaceName(VerifyFace face)
{
    switch (face) {
        case VerifyFace::Roots: return "roots";
        case VerifyFace::Objects: return "objects";
        case VerifyFace::Marking: return "marking";
        case VerifyFace::Remembered: return "remembered";
        case VerifyFace::Oops: return "oops";
        default: return "unknown";
    }
}

bool VerifyFaceEnabled(VerifyFace face)
{
    switch (face) {
        // ZGC verification flags are runtime constants. Preserve that one-time
        // read without letting the first queried face freeze the other four.
        case VerifyFace::Roots: {
            static const bool enabled =
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_ROOTS")) || DiagGate::TokenOn("roots");
            return enabled;
        }
        case VerifyFace::Objects: {
            static const bool enabled = ValueIsOne(std::getenv("MRT_GCV2_VERIFY_HEAP")) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_OBJECTS")) || DiagGate::TokenOn("objects");
            return enabled;
        }
        case VerifyFace::Marking: {
            static const bool enabled = ValueIsOne(std::getenv("MRT_GCV2_MARKCOMPLETE")) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_MARKING")) || DiagGate::TokenOn("marking");
            return enabled;
        }
        case VerifyFace::Remembered: {
            static const bool enabled = ValueIsOne(std::getenv("MRT_GCV2_VERIFY_REMSET")) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_REMEMBERED")) || DiagGate::TokenOn("remembered");
            return enabled;
        }
        case VerifyFace::Oops: {
            static const bool enabled = ValueIsOne(std::getenv("MRT_GCV2_VERIFY_REGIONS")) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_OOPS")) || DiagGate::TokenOn("oops");
            return enabled;
        }
        default:
            return false;
    }
}

bool VerifyPhaseEnter(VerifyFace face, const char* phase)
{
    if (!VerifyFaceEnabled(face)) {
        return false;
    }
    DiagGate::MaybeAnnounce();
    VLOG(REPORT, "[GCV2][verify] face=%s phase=%s detail=phase-entry", VerifyFaceName(face),
         phase == nullptr ? "?" : phase);
    return true;
}
} // namespace MapleRuntime
