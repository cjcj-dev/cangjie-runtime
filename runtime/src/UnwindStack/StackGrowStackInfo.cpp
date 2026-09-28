// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "StackGrowStackInfo.h"

#include <stack>

#include "Heap/z/zMark.hpp"
#include "Common/StackType.h"
#include "Interpreter/InterpreterSpecific.h"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {
void StackGrowStackInfo::FillInStackTrace()
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return;
    }
    UnwindContext uwContext;
    // Top unwind context can only be runtime or Cangjie context.
    CheckTopUnwindContextAndInit(uwContext);

#ifdef _WIN64
    // The offset on the window is unchanged.
    const uintptr_t firstFrameBaseOffset = 48;
    while (reinterpret_cast<uintptr_t>(uwContext.frameInfo.mFrame.GetFA()) !=
           mutator->GetStackBaseAddr() - firstFrameBaseOffset) {
#else
    // [rbp] of CJ_CJThreadEntry is 0x0.
    while (reinterpret_cast<uintptr_t>(uwContext.frameInfo.mFrame.GetFA()) != 0) {
#endif
        AnalyseAndSetFrameType(uwContext);
        ProcessOnIteration(uwContext.frameInfo);
        stack.emplace_back(uwContext.frameInfo);
        UnwindContext caller;
        lastFrameType = uwContext.frameInfo.GetFrameType();
#ifndef _WIN64
        if (uwContext.UnwindToCallerContext(caller) == false) {
#else
        if (uwContext.UnwindToCallerContext(caller, uwCtxStatus) == false) {
#endif
            return;
        }
        caller.frameInfo.mFrame.SetSP(uwContext.frameInfo.CallerSP());
        uwContext = caller;
    }
}

void StackGrowStackInfo::RecordStackPtrsImpl(const StackPtrVisitor& traceAndFixPtrVisitor,
                                             const StackPtrVisitor& fixPtrVisitor,
                                             const DerivedPtrVisitor& derivedPtrVisitor,
                                             RegSlotsMap& regSlotsMap,
                                             const FrameInfo& frame, Mutator& mutator)
{
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackPtrMap stackPtrMap = StackMapBuilder(startIP, frameIP, frameAddress).Build<StackPtrMap>(true);
    if (stackPtrMap.IsValid()) {
        if (!regSlotsMap.allRegistersSaved && stackPtrMap.HasGCRegisterRoots()) {
            LOG(RTLOG_FATAL, "GC register root at ordinary statepoint, frame pc: %p",
                reinterpret_cast<void*>(frameIP));
        }
        if (!stackPtrMap.VisitReg(traceAndFixPtrVisitor, fixPtrVisitor, nullptr, regSlotsMap)) {
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
        stackPtrMap.VisitSlot(traceAndFixPtrVisitor, fixPtrVisitor, nullptr);
        stackPtrMap.VisitDerivedPtr(derivedPtrVisitor, regSlotsMap);
    }
}

void StackGrowStackInfo::RecordStackPtrs(const StackPtrVisitor& traceAndFixPtrVisitor,
                                         const StackPtrVisitor& fixPtrVisitor,
                                         const DerivedPtrVisitor& derivedPtrVisitor, Mutator& mutator)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    RegisterMap() = RegSlotsMap();
    for (const auto& frame : stack) {
        ObjectRef* rbp = reinterpret_cast<ObjectRef*>(frame.GetMachineFrame().GetFA());
        fixPtrVisitor(*rbp);
        if (frame.GetFrameType() == FrameType::MANAGED) {
            RecordStackPtrsImpl(traceAndFixPtrVisitor, fixPtrVisitor, derivedPtrVisitor,
                                RegisterMap(), frame, mutator);
        }
        PublishCalleeRegisters(frame);
    }

#ifdef INTERPRETER_ENABLED
    auto expansionStackVisitor = [this, &fixPtrVisitor, &derivedPtrVisitor](DYN_VisitingState state) {
        for (const auto& frame : stack) {
            switch (frame.GetFrameType()) {
                case FrameType::INTERPRETER:
                    VisitInterpreterFrameRootsExpansion(state, frame, &fixPtrVisitor, &derivedPtrVisitor);
                    break;
                default: {
                    break;
                }
            }
        }
    };

    IterateFramesWithState(mutator.interpreterCJThreadData, [](DYN_VisitingState state, void* ctx) {
        auto closure = reinterpret_cast<decltype(expansionStackVisitor)*>(ctx);
        (*closure)(state);
    }, &expansionStackVisitor);
#endif
}
} // namespace MapleRuntime
