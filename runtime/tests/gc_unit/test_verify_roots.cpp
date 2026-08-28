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

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr Uptr kFakeHeapAddress = Uptr(0x00007f12'34567000ULL);

size_t RunColouredRootArm(bool enabled)
{
    if (enabled) {
        setenv("MRT_GCV2_VERIFY_ROOTS", "1", 1);
    } else {
        unsetenv("MRT_GCV2_VERIFY_ROOTS");
    }

    VerifyRoots::ResetStats();
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
    return VerifyRoots::BadRootCount();
}

} // namespace

GC_OTHER_VM_TEST(VerifyRoots, DefaultOffLeavesCorruptRootUnobserved)
{
#if defined(__linux__)
    GC_EXPECT_EQ(RunColouredRootArm(false), 0u);
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(VerifyRoots, EnabledReportsColouredRootBeforeDereference)
{
#if defined(__linux__)
    GC_EXPECT_EQ(RunColouredRootArm(true), 1u);
#else
    GC_EXPECT_TRUE(true);
#endif
}
