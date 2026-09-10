// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Common/ColourMask.h"
#include "Common/ColourTypes.h"
#include "Heap/Verify/VerifyRoots.h"
#include "gc_unittest.hpp"

#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr Uptr kFakeHeapAddress = Uptr(0x00007f12'34567000ULL);

size_t RunColouredRootArm(bool enabled, bool closeScene)
{
    if (enabled) {
        setenv("MRT_GCV2_VERIFY_ROOTS", "1", 1);
    } else {
        unsetenv("MRT_GCV2_VERIFY_ROOTS");
    }

    VerifyRoots::ResetStats();
    VerifyRoots::BeginScene("gc-unit-coloured-root");
    RootVerifyContext ctx;
    ctx.phase = "gc_unit-coloured-root";
    ctx.kind = RootKind::RUNTIME_ROOT;
    const Uptr corrupt = kFakeHeapAddress | ZPointerRemapped00;
    // Deliberate corruption: copy a coloured HeapSlot word into actual
    // RootSlot storage, bypassing the typed writer that forbids this.
    RootSlot badRoot;
    static_assert(sizeof(badRoot) == sizeof(corrupt), "root slot is one word");
    std::memcpy(&badRoot, &corrupt, sizeof(corrupt));
    ctx.rawValue = raw(badRoot.LoadPlain());
    ctx.hasRawValue = true;
    VerifyRoots::VerifyRootPayload(ctx, &badRoot, nullptr);
    if (closeScene) {
        VerifyRoots::EndScene("gc-unit-coloured-root");
    }
    return VerifyRoots::BadRootCount();
}

} // namespace

GC_OTHER_VM_TEST(VerifyRoots, DefaultOffLeavesCorruptRootUnobserved)
{
#if defined(__linux__)
    GC_EXPECT_EQ(RunColouredRootArm(false, true), 0u);
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(VerifyRoots, EnabledClosesColouredRootScene)
{
#if defined(__linux__)
    int childStderr[2];
    GC_EXPECT_EQ(pipe(childStderr), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(childStderr[0]);
        GC_EXPECT_TRUE(dup2(childStderr[1], STDERR_FILENO) >= 0);
        close(childStderr[1]);
        (void)signal(SIGABRT, SIG_DFL);
        (void)RunColouredRootArm(true, true);
        _exit(0);
    }
    close(childStderr[1]);
    std::string transcript;
    char buffer[512];
    for (;;) {
        const ssize_t count = read(childStderr[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        transcript.append(buffer, static_cast<size_t>(count));
    }
    close(childStderr[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(transcript.find("[GCV2][verify][roots] scene failed") != std::string::npos);
#else
    GC_EXPECT_TRUE(true);
#endif
}
