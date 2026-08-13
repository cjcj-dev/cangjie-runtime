// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Product regressions for hunt-stackmap BUG 1/3/4. Each case calls the shipped
// function. Before the matching commit the case either OOB-reads, overlaps
// sprintf_s dest/%s, or leaves fallback pc/line uninitialized.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#endif

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

#if defined(__linux__)
size_t overlappingSprintfCallCount = 0;
size_t signalFrameSprintfCallCount = 0;

bool FirstStringArgumentOverlapsDestination(char* dest, size_t destMax, const char* format, va_list args)
{
    if (dest == nullptr || format == nullptr || format[0] != '%' || format[1] != 's') {
        return false;
    }
    va_list inspect;
    va_copy(inspect, args);
    const char* firstString = va_arg(inspect, const char*);
    va_end(inspect);
    uintptr_t destAddress = reinterpret_cast<uintptr_t>(dest);
    uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(firstString);
    return sourceAddress >= destAddress && sourceAddress - destAddress < destMax;
}

bool CalledDirectlyBySignalFramePrint(void* returnAddress)
{
    Dl_info callerInfo {};
    return dladdr(returnAddress, &callerInfo) != 0 && callerInfo.dli_sname != nullptr &&
        std::strcmp(callerInfo.dli_sname, "_ZNK12MapleRuntime19SigHandlerFrameinfo14PrintFrameInfoEj") == 0;
}
#endif

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

// Interpose the product's dynamically linked sprintf_s without changing its
// result.  The counter makes the former overlapping dest/first-%s call shape a
// direct, implementation-independent assertion instead of relying on a
// particular securec version to reject it.
#if defined(__linux__)
extern "C" int sprintf_s(char* dest, size_t destMax, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    void* returnAddress = __builtin_extract_return_addr(__builtin_return_address(0));
    if (CalledDirectlyBySignalFramePrint(returnAddress)) {
        ++signalFrameSprintfCallCount;
        if (FirstStringArgumentOverlapsDestination(dest, destMax, format, args)) {
            ++overlappingSprintfCallCount;
        }
    }
    int rc = vsprintf_s(dest, destMax, format, args);
    va_end(args);
    return rc;
}
#endif

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

// BUG 3: pre-fix PrintFrameInfo passed its destination as the first %s source.
// Count that product call shape directly; securec versions differ on whether
// they reject it, so a securec return value is not a product regression test.
GC_TEST(UnwindRegress, SignalFrameAppendDoesNotOverlap)
{
#if defined(__linux__)
    overlappingSprintfCallCount = 0;
    signalFrameSprintfCallCount = 0;
#endif
    SigHandlerFrameinfo frame;
    frame.mFrame.SetIP(reinterpret_cast<const uint32_t*>(&sprintf_s));
    frame.SetFrameType(FrameType::NATIVE);
    frame.PrintFrameInfo(0);
#if defined(__linux__)
    std::printf("SIGNAL_FRAME_DIRECT_SPRINTF_CALLS=%zu SIGNAL_FRAME_OVERLAPPING_SPRINTF_CALLS=%zu\n",
                signalFrameSprintfCallCount, overlappingSprintfCallCount);
    GC_EXPECT_NE(signalFrameSprintfCallCount, 0u);
    GC_EXPECT_EQ(overlappingSprintfCallCount, 0u);
#endif
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
