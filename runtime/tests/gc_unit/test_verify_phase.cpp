// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/VerifyPhase.h"
#include "gc_unittest.hpp"

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdlib>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(__linux__)
namespace {
int CheckFace(VerifyFace face, const char* legacy, const char* alias, const char* token, int mode)
{
    const pid_t child = fork();
    if (child == 0) {
        const char* names[] = {"MRT_GCV2_VERIFY_ROOTS", "MRT_GCV2_VERIFY_HEAP", "MRT_GCV2_VERIFY_OBJECTS",
                               "MRT_GCV2_MARKCOMPLETE", "MRT_GCV2_VERIFY_MARKING", "MRT_GCV2_VERIFY_REMSET",
                               "MRT_GCV2_VERIFY_REMEMBERED", "MRT_GCV2_VERIFY_REGIONS", "MRT_GCV2_VERIFY_OOPS",
                               "MRT_GCV2_DIAG"};
        for (const char* name : names) {
            unsetenv(name);
        }
        if (mode == 1) {
            setenv(legacy, "1", 1);
        } else if (mode == 2) {
            setenv(alias, "1", 1);
        } else if (mode == 3) {
            setenv("MRT_GCV2_DIAG", token, 1);
        }
        _exit(VerifyFaceEnabled(face) == (mode != 0) ? 0 : 1);
    }
    if (child < 0) {
        return -1;
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace
#endif

GC_TEST(VerifyPhase, FiveFaceLegacyAliasAndTokenMatrix)
{
#if defined(__linux__)
    struct Case {
        VerifyFace face;
        const char* legacy;
        const char* alias;
        const char* token;
    };
    const Case cases[] = {{VerifyFace::Roots, "MRT_GCV2_VERIFY_ROOTS", "MRT_GCV2_VERIFY_ROOTS", "roots"},
                          {VerifyFace::Objects, "MRT_GCV2_VERIFY_HEAP", "MRT_GCV2_VERIFY_OBJECTS", "objects"},
                          {VerifyFace::Marking, "MRT_GCV2_MARKCOMPLETE", "MRT_GCV2_VERIFY_MARKING", "marking"},
                          {VerifyFace::Remembered, "MRT_GCV2_VERIFY_REMSET", "MRT_GCV2_VERIFY_REMEMBERED",
                           "remembered"},
                          {VerifyFace::Oops, "MRT_GCV2_VERIFY_REGIONS", "MRT_GCV2_VERIFY_OOPS", "oops"}};
    for (const auto& item : cases) {
        GC_EXPECT_EQ(CheckFace(item.face, item.legacy, item.alias, item.token, 0), 0);
        GC_EXPECT_EQ(CheckFace(item.face, item.legacy, item.alias, item.token, 1), 0);
        GC_EXPECT_EQ(CheckFace(item.face, item.legacy, item.alias, item.token, 2), 0);
        GC_EXPECT_EQ(CheckFace(item.face, item.legacy, item.alias, item.token, 3), 0);
    }
#else
    GC_EXPECT_TRUE(true);
#endif
}
