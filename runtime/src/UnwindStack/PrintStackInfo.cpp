// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "PrintStackInfo.h"

#include "Common/StackType.h"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {
void PrintStackInfo::FillInStackTrace()
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    UnwindContext uwContext;
    // Top unwind context can only be runtime or Cangjie context.
    CheckTopUnwindContextAndInit(uwContext);
    while (!uwContext.frameInfo.mFrame.IsAnchorFrame(anchorFA)) {
        UnwindContext caller;
        if (uwContext.frameInfo.mFrame.GetFA() == nullptr) {
            (void)ReadCallerFrame(uwContext, caller);
            return;
        }
        AnalyseAndSetFrameType(uwContext);
        ProcessOnIteration(uwContext.frameInfo);

        FrameType currentFrameType = uwContext.frameInfo.GetFrameType();
        if (currentFrameType == FrameType::MANAGED) {
            stack.emplace_back(uwContext.frameInfo);
        }
#ifdef INTERPRETER_ENABLED
        if (currentFrameType == FrameType::INTERPRETER) {
            stack.emplace_back(uwContext.frameInfo);
        }
#endif

        if (!ReadCallerFrame(uwContext, caller)) {
            return;
        }
        uwContext = caller;
    }
}

void PrintStackInfo::PrintStackTrace() const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    size_t frameSize = stack.size();
    for (size_t i = 0; i < frameSize; ++i) {
        stack[i].PrintFrameInfo(i);
    }
#endif
}

#if defined(__IOS__)
CString PrintStackInfo::GetStackTraceString()
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    UnwindContext uwContext;
    // Top unwind context can only be runtime or Cangjie context.
    CheckTopUnwindContextAndInit(uwContext);
    while (!uwContext.frameInfo.mFrame.IsAnchorFrame(anchorFA)) {
        UnwindContext caller;
        if (uwContext.frameInfo.mFrame.GetFA() == nullptr) {
            (void)ReadCallerFrame(uwContext, caller);
            break;
        }
        AnalyseAndSetFrameType(uwContext);
        ProcessOnIteration(uwContext.frameInfo);
        stack.emplace_back(uwContext.frameInfo);
        if (!ReadCallerFrame(uwContext, caller)) {
            break;
        }
        uwContext = caller;
    }

    CString res;
    size_t frameSize = stack.size();
    for (size_t i = 0; i < frameSize; ++i) {
        res.Append(stack[i].GetFrameInfo(i));
    }
    return res;
}
#endif
} // namespace MapleRuntime
