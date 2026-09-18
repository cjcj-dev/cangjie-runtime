// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackFrameCursor.h"

#include "Heap/z/zMark.hpp"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {

namespace {
// Local fill that mirrors GCStackInfo::FillInStackTrace without depending on GCStackInfo layout.
class CursorFillStackInfo : public StackInfo {
public:
    explicit CursorFillStackInfo(const UnwindContext* context) : StackInfo(context) {}
    void FillInStackTrace() override
    {
        ElfUnloadQuiescence::ReadScope metadataReader;
        UnwindContext uwContext;
        CheckTopUnwindContextAndInit(uwContext);
        while (!uwContext.frameInfo.mFrame.IsAnchorFrame(anchorFA)) {
            AnalyseAndSetFrameType(uwContext);
            stack.emplace_back(uwContext.frameInfo);
            UnwindContext caller;
            lastFrameType = uwContext.frameInfo.GetFrameType();
#ifndef _WIN64
            if (uwContext.UnwindToCallerContext(caller) == false) {
#else
            if (uwContext.UnwindToCallerContext(caller, uwCtxStatus) == false) {
#endif
                LOG(RTLOG_ERROR,
                    "StackFrameCursor unwind truncated at frames=%zu ip=%p fa=%p",
                    stack.size(), uwContext.frameInfo.mFrame.GetIP(), uwContext.frameInfo.mFrame.GetFA());
                return;
            }
            uwContext = caller;
        }
    }
    using StackInfo::stack;
};
} // namespace

StackFrameCursor::StackFrameCursor(const UnwindContext& topFrame)
{
    CursorFillStackInfo filler(&topFrame);
    filler.FillInStackTrace();
    frames = std::move(filler.stack);
}

void StackFrameCursor::ProcessFrame(const FrameInfo& frame, RegSlotsMap& regSlotsMap, const RootVisitor& visitor,
                                    Mutator& mutator, const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
#ifdef __arm__
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            (void)young;
            CopyCollector::Process(visitor, derivedPtrVisitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::STACKGROW:
            LOG(RTLOG_FATAL, "STACKGROW frame is not supported in Process");
            break;
        case FrameType::SAFEPOINT:
            CopyCollector::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
            CopyCollector::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2N_STUB:
            CopyCollector::RecordC2NStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::EXSLUSIVE:
            CopyCollector::RecordExclusiveStubCalleeSaved(regSlotsMap,
                                                             reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
#else
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            (void)young;
            CopyCollector::Process(visitor, derivedPtrVisitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::SAFEPOINT:
        case FrameType::STACKGROW:
            CopyCollector::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
        case FrameType::C2N_STUB:
        case FrameType::EXSLUSIVE:
#ifdef INTERPRETER_ENABLED
        case FrameType::INTERPRETER_C2I:
#endif
            CopyCollector::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
    (void)mutator;
#endif
}

bool StackFrameCursor::ProcessOne(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    if (Done()) {
        return false;
    }

    ProcessFrame(frames[index], regSlotsMap, visitor, mutator, derivedPtrVisitor, young);
    ++index;
    return true;
}

void StackFrameCursor::ProcessAll(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    while (ProcessOne(visitor, mutator, derivedPtrVisitor, young)) {
    }
}

} // namespace MapleRuntime
