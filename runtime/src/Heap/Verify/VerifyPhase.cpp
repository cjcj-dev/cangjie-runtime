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

// Match the ZGC trueInDebug defaults whose verifier semantics are present:
// debug builds enable roots and remembered-set verification. Marking stays
// opt-in until the marking-stack verifier is provided; today's Marking face
// verifies object closure instead. Product builds keep every optional face off,
// while objects and per-oop verification remain opt-in (z_globals.hpp:78-119).
bool BuildDefault(VerifyFace face)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    return face == VerifyFace::Roots || face == VerifyFace::Remembered;
#else
    (void)face;
    return false;
#endif
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
                BuildDefault(face) || ValueIsOne(std::getenv("MRT_GCV2_VERIFY_ROOTS")) ||
                DiagGate::TokenOn("roots");
            return enabled;
        }
        case VerifyFace::Objects: {
            static const bool enabled = BuildDefault(face) || ValueIsOne(std::getenv("MRT_GCV2_VERIFY_HEAP")) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_OBJECTS")) || DiagGate::TokenOn("objects");
            return enabled;
        }
        case VerifyFace::Marking: {
            static const bool enabled = BuildDefault(face) ||
                ValueIsOne(std::getenv("MRT_GCV2_VERIFY_MARKING")) || DiagGate::TokenOn("marking");
            return enabled;
        }
        case VerifyFace::Remembered: {
            static const bool enabled = BuildDefault(face) || DiagGate::TokenOn("remembered");
            return enabled;
        }
        case VerifyFace::Oops: {
            static const bool enabled = BuildDefault(face) || ValueIsOne(std::getenv("MRT_GCV2_VERIFY_REGIONS")) ||
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
