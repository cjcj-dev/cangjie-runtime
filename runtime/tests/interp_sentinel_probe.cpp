// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// BUG 2 probe: batch lite-frame decode of an interpreter sentinel {fuh, bcPos, 0}.
// Pre-fix: StackMetadataHelper ctor dereferences funcDesc=0 (SEGV) before the
// sentinel branch. Post-fix: reaches FillInterpretedFrameDesc; without an
// interpreter that is a FATAL on the uninitialized interface (proves the
// unused helper is gone). With the mock .so, the frame is filled.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Cangjie.h"
#include "Common/StackType.h"
#include "Interpreter/RTInterface.h"
#include "UnwindStack/StackInfo.h"

using namespace MapleRuntime;

#ifdef SM4_INTERP_MOCK
static void FreeDesc(char* methodName, char* className, char* fileName)
{
    std::free(methodName);
    std::free(className);
    std::free(fileName);
}

static void MockFrameDesc(INT_FunctionHandle, INT_BytecodePos, INT_InterpretedFrameDesc* frameDesc)
{
    frameDesc->lineNumber = 17;
    frameDesc->methodName = strdup("interpMethod");
    frameDesc->className = strdup("interpClass");
    frameDesc->fileName = strdup("interp.cj");
    frameDesc->freeResources = &FreeDesc;
}

extern "C" int interpreter_bridge_init(INT_InterpreterInterface* interpreterInterface,
                                       DYN_CJNativeInterface*, int, INT_InterpreterArgs)
{
    if (interpreterInterface == nullptr) {
        return 1;
    }
    interpreterInterface->version = INT_INTERPRETER_INTERFACE_VERSION;
    interpreterInterface->frameDescProvider = &MockFrameDesc;
    return 0;
}
#endif

#ifndef SM4_INTERP_MOCK
int main()
{
    std::vector<uint64_t> lite = { 0x11, 0x22, 0 };
    std::vector<StackTraceElement> trace;
    StackInfo::GetStackTraceByLiteFrameInfos(lite, trace);
    std::fprintf(stderr, "INTERP_SENTINEL_REACHED frames=%zu\n", trace.size());
    return 0;
}
#endif
