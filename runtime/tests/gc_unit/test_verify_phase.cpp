// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/VerifyPhase.h"
#include "gc_unittest.hpp"

#include <cstdio>
#include <cstdlib>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct FaceCase {
    VerifyFace face;
    const char* legacy;
    const char* alias;
};

const FaceCase kFaces[] = {{VerifyFace::Roots, "MRT_GCV2_VERIFY_ROOTS", "MRT_GCV2_VERIFY_ROOTS"},
                           {VerifyFace::Objects, "MRT_GCV2_VERIFY_HEAP", "MRT_GCV2_VERIFY_OBJECTS"},
                           {VerifyFace::Marking, "MRT_GCV2_VERIFY_MARKING", "MRT_GCV2_VERIFY_MARKING"},
                           {VerifyFace::Remembered, "MRT_GCV2_VERIFY_REMSET", "MRT_GCV2_VERIFY_REMEMBERED"},
                           {VerifyFace::Oops, "MRT_GCV2_VERIFY_REGIONS", "MRT_GCV2_VERIFY_OOPS"}};

void ClearFaceEnvironment()
{
    for (const auto& item : kFaces) {
        unsetenv(item.legacy);
        unsetenv(item.alias);
    }
    unsetenv("MRT_GCV2_DIAG");
}

void ExpectAllFaces(bool enabled)
{
    for (const auto& item : kFaces) {
        GC_EXPECT_EQ(VerifyFaceEnabled(item.face), enabled);
    }
}
} // namespace

GC_OTHER_VM_TEST(VerifyPhase, FiveFaceBuildDefaultMatrix)
{
#if defined(__linux__)
    ClearFaceEnvironment();
    const bool roots = VerifyFaceEnabled(VerifyFace::Roots);
    const bool objects = VerifyFaceEnabled(VerifyFace::Objects);
    const bool marking = VerifyFaceEnabled(VerifyFace::Marking);
    const bool remembered = VerifyFaceEnabled(VerifyFace::Remembered);
    const bool oops = VerifyFaceEnabled(VerifyFace::Oops);
    std::fprintf(stderr, "VERIFY_PHASE_DEFAULT_MATRIX roots=%d objects=%d marking=%d remembered=%d oops=%d\n",
                 roots, objects, marking, remembered, oops);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    GC_EXPECT_FALSE(objects);
    // Marking remains opt-in until #65 supplies a marking-stack verifier.
    GC_EXPECT_FALSE(marking);
    GC_EXPECT_TRUE(remembered);
    GC_EXPECT_FALSE(oops);
    std::fprintf(stderr, "VERIFY_PHASE_DEFAULT_MATRIX_PRIOR_ASSERTIONS_EXECUTED count=4\n");
    GC_EXPECT_TRUE(roots);
    std::fprintf(stderr, "VERIFY_PHASE_DEFAULT_MATRIX_ASSERTIONS_EXECUTED mode=debug\n");
#else
    GC_EXPECT_FALSE(roots);
    GC_EXPECT_FALSE(objects);
    GC_EXPECT_FALSE(marking);
    GC_EXPECT_FALSE(remembered);
    GC_EXPECT_FALSE(oops);
    std::fprintf(stderr, "VERIFY_PHASE_DEFAULT_MATRIX_ASSERTIONS_EXECUTED mode=release\n");
#endif
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(VerifyPhase, FiveFaceLegacyArm)
{
    ClearFaceEnvironment();
    for (const auto& item : kFaces) {
        setenv(item.legacy, "1", 1);
    }
    ExpectAllFaces(true);
}

GC_OTHER_VM_TEST(VerifyPhase, FiveFaceAliasArm)
{
    ClearFaceEnvironment();
    for (const auto& item : kFaces) {
        setenv(item.alias, "1", 1);
    }
    ExpectAllFaces(true);
}

GC_OTHER_VM_TEST(VerifyPhase, FiveFaceTokenArm)
{
    ClearFaceEnvironment();
    setenv("MRT_GCV2_DIAG", "roots,objects,marking,remembered,oops", 1);
    ExpectAllFaces(true);
}

GC_OTHER_VM_TEST(VerifyPhase, QueryingOneFaceDoesNotFreezeAnother)
{
    ClearFaceEnvironment();
    GC_EXPECT_FALSE(VerifyFaceEnabled(VerifyFace::Objects));
    setenv("MRT_GCV2_VERIFY_OOPS", "1", 1);
    GC_EXPECT_TRUE(VerifyFaceEnabled(VerifyFace::Oops));
}

GC_OTHER_VM_TEST(VerifyPhase, LateSetenvDoesNotRetuneInitializedFace)
{
    ClearFaceEnvironment();
    GC_EXPECT_FALSE(VerifyFaceEnabled(VerifyFace::Objects));
    setenv("MRT_GCV2_VERIFY_OBJECTS", "1", 1);
    GC_EXPECT_FALSE(VerifyFaceEnabled(VerifyFace::Objects));
}
