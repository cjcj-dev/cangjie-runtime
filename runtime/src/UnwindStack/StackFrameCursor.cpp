// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackFrameCursor.h"

#include "Collector/TracingCollector.h"

namespace MapleRuntime {

namespace {
// Local fill that mirrors GCStackInfo::FillInStackTrace without depending on GCStackInfo layout.
class CursorFillStackInfo : public StackInfo {
public:
    explicit CursorFillStackInfo(const UnwindContext* context) : StackInfo(context) {}
    void FillInStackTrace() override
    {
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
                                    Mutator& mutator)
{
#ifdef __arm__
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            TracingCollector::VisitStackRoots(visitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::STACKGROW:
            LOG(RTLOG_FATAL, "STACKGROW frame is not supported in VisitStackRoots");
            break;
        case FrameType::SAFEPOINT:
            TracingCollector::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
            TracingCollector::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2N_STUB:
            TracingCollector::RecordC2NStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::EXSLUSIVE:
            TracingCollector::RecordExclusiveStubCalleeSaved(regSlotsMap,
                                                             reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
#else
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            TracingCollector::VisitStackRoots(visitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::SAFEPOINT:
        case FrameType::STACKGROW:
            TracingCollector::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
        case FrameType::C2N_STUB:
        case FrameType::EXSLUSIVE:
#ifdef INTERPRETER_ENABLED
        case FrameType::INTERPRETER_C2I:
#endif
            TracingCollector::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
    (void)mutator;
#endif
}

bool StackFrameCursor::ProcessOne(const RootVisitor& visitor, Mutator& mutator)
{
    if (Done()) {
        return false;
    }
    ProcessFrame(frames[index], regSlotsMap, visitor, mutator);
    ++index;
    return true;
}

void StackFrameCursor::ProcessAll(const RootVisitor& visitor, Mutator& mutator)
{
    while (ProcessOne(visitor, mutator)) {
    }
}

bool StackFrameCursor::SkipNextManagedFrame()
{
    // Advance past the next MANAGED frame without visiting its roots.
    // Stub frames before that MANAGED frame must already have been processed via ProcessOne
    // so RegSlotsMap remains consistent with the legacy walker.
    while (!Done()) {
        if (frames[index].GetFrameType() == FrameType::MANAGED) {
            ++index;
            return true;
        }
        ++index;
    }
    return false;
}

} // namespace MapleRuntime
