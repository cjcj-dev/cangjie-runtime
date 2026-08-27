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
bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool LegacyOrAlias(const char* legacy, const char* alias, const char* token)
{
    return EnvIsOne(legacy) || EnvIsOne(alias) || DiagGate::TokenOn(token);
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
    static const bool roots = LegacyOrAlias("MRT_GCV2_VERIFY_ROOTS", "MRT_GCV2_VERIFY_ROOTS", "roots");
    static const bool objects = LegacyOrAlias("MRT_GCV2_VERIFY_HEAP", "MRT_GCV2_VERIFY_OBJECTS", "objects");
    static const bool marking = LegacyOrAlias("MRT_GCV2_MARKCOMPLETE", "MRT_GCV2_VERIFY_MARKING", "marking");
    static const bool remembered = LegacyOrAlias("MRT_GCV2_VERIFY_REMSET", "MRT_GCV2_VERIFY_REMEMBERED", "remembered");
    static const bool oops = LegacyOrAlias("MRT_GCV2_VERIFY_REGIONS", "MRT_GCV2_VERIFY_OOPS", "oops");
    switch (face) {
        case VerifyFace::Roots:
            return roots;
        case VerifyFace::Objects:
            return objects;
        case VerifyFace::Marking:
            return marking;
        case VerifyFace::Remembered:
            return remembered;
        case VerifyFace::Oops:
            return oops;
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
