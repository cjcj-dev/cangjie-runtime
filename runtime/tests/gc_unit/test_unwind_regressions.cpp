// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Product regressions for hunt-stackmap BUG 1/3/4. Each case calls the shipped
// function. Before the matching commit the case either OOB-reads, overlaps
// sprintf_s dest/%s, or leaves fallback pc/line uninitialized.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "CangjieRuntime.h"
#include "Common/StackType.h"
#include "Exception/Exception.h"
#include "ObjectModel/MFuncdesc.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/StackInfo.h"
#include "gc_unittest.hpp"
#include "securec.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct FakeFuncDescLayout {
    int32_t stackMapOff;
    uint32_t codeSize;
    uint32_t name;
    uint32_t directory;
    uint32_t filename;
    uint32_t dictOffsets;
    int32_t ehTableOff;
    char empty;
    int8_t formatByte;
    uint8_t stackmap[64];
};

void InitEmptyFuncDesc(FakeFuncDescLayout* blob)
{
    std::memset(blob, 0, sizeof(*blob));
    blob->name = static_cast<uint32_t>(offsetof(FakeFuncDescLayout, empty));
    blob->directory = static_cast<uint32_t>(offsetof(FakeFuncDescLayout, empty));
    blob->filename = static_cast<uint32_t>(offsetof(FakeFuncDescLayout, empty));
    blob->dictOffsets = static_cast<uint32_t>(offsetof(FakeFuncDescLayout, formatByte) + 1);
    blob->stackMapOff = static_cast<int32_t>(offsetof(FakeFuncDescLayout, stackmap));
}

} // namespace

// BUG 1: 3n+1 SOF flag must not be decoded as a frame (StackInfo.cpp).
// Pre-fix walked i=3 and read liteFrameInfos[4], [5].
GC_TEST(UnwindRegress, SofFoldFlagIsNotAFrame)
{
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
    FakeFuncDescLayout blob;
    InitEmptyFuncDesc(&blob);
    std::vector<uint64_t> lite;
    lite.push_back(0x1000);
    lite.push_back(0x1000);
    lite.push_back(reinterpret_cast<uint64_t>(&blob));
    lite.push_back(static_cast<uint64_t>(SofStackFlag::BOTTOM_FOLDED));
    GC_EXPECT_EQ(lite.size() % 3, 1u);

    std::vector<StackTraceElement> trace;
    StackInfo::GetStackTraceByLiteFrameInfos(lite, trace);
    GC_EXPECT_EQ(trace.size(), 1u);
}

// BUG 3: overlapping sprintf_s dest/%s is rejected by this securec.
// Pre-fix PrintFrameInfo used that shape on fileName and outputStr.
GC_TEST(UnwindRegress, OverlappingSprintfIsRejected)
{
    char buf[64];
    GC_EXPECT_TRUE(sprintf_s(buf, sizeof(buf), "%s", "dir") != -1);
    int rc = sprintf_s(buf, sizeof(buf), "%s%s", buf, "/file.cj");
    GC_EXPECT_EQ(rc, -1);
}

// BUG 3 after-fix: native signal-frame print appends without overlap and does not abort.
GC_TEST(UnwindRegress, SignalFramePrintDoesNotAbort)
{
    SigHandlerFrameinfo frame;
    frame.mFrame.SetIP(reinterpret_cast<const uint32_t*>(&sprintf_s));
    frame.SetFrameType(FrameType::NATIVE);
    frame.PrintFrameInfo(0);
}

// BUG 4: fallback must write pc/line 0, not leave caller sentinels.
// Pre-fix left 0xA5A5A5A5 in both slots.
GC_TEST(UnwindRegress, FallbackZeroesPcAndLine)
{
    UnwindContext ctx;
    CJThreadStackInfo planted(&ctx, 256);
    FrameInfo emptyNative;
    emptyNative.SetFrameType(FrameType::NATIVE);
    planted.GetStack().push_back(emptyNative);

    uint32_t pcs[TRACE_STACK_MAX_DEPTH];
    uint32_t lines[TRACE_STACK_MAX_DEPTH];
    char* funcs[TRACE_STACK_MAX_DEPTH];
    char* files[TRACE_STACK_MAX_DEPTH];
    std::memset(pcs, 0xA5, sizeof(pcs));
    std::memset(lines, 0xA5, sizeof(lines));
    std::memset(funcs, 0, sizeof(funcs));
    std::memset(files, 0, sizeof(files));

    planted.GetInfoFromStackTrace(pcs, funcs, files, lines);
    GC_EXPECT_EQ(planted.GetRealStackSize(), 1);
    GC_EXPECT_EQ(pcs[0], 0u);
    GC_EXPECT_EQ(lines[0], 0u);
    GC_EXPECT_TRUE(funcs[0] != nullptr && std::strcmp(funcs[0], "?") == 0);
    GC_EXPECT_TRUE(files[0] != nullptr && std::strcmp(files[0], "unknown") == 0);
    std::free(funcs[0]);
    std::free(files[0]);
}
