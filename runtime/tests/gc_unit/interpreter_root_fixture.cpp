// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// A finite ABI consumer, not an implementation of the external interpreter.
#include <cstddef>
#include <cstdint>
#include "Interpreter/RuntimeAPI.h"
#include "Interpreter/RTInterface.h"

namespace {
DYN_CJNativeInterface native;
void (*startHook)(void*, void*) = nullptr;
void* hookContext = nullptr;
void Start(DYN_CJThreadSpecificData* field)
{
    *field = nullptr;
    if (startHook != nullptr) {
        startHook(hookContext, reinterpret_cast<char*>(field) - native.cjThreadSpecificOffset);
    }
}
void Destroy(DYN_CJThreadSpecificData*) {}
void Iterate(DYN_CJThreadSpecificData, void (*visit)(DYN_VisitingState, void*), void* context)
{
    size_t visited = 0;
    visit(&visited, context);
}
void Globals(DYN_RootVisitor) {}
void Mark(DYN_VisitingState state, INT_FrameDesc frame, DYN_RootVisitor visitor)
{
    ++*static_cast<size_t*>(state);
    // Test frames contain the native frame header followed by an uncolored root.
    native.visitRootFromInterpreter(visitor, const_cast<uintptr_t*>(static_cast<const uintptr_t*>(frame.fp) + 2));
}
void Adjust(DYN_VisitingState state, INT_FrameDesc frame, DYN_RootVisitor root, DYN_DerivedPtrVisitor)
{
    Mark(state, frame, root);
}
}
extern "C" void GCInterpreterSetStartHook(void (*hook)(void*, void*), void* context)
{
    startHook = hook;
    hookContext = context;
}
extern "C" void GCInterpreterFramePC() {}
extern "C" int interpreter_bridge_init(INT_InterpreterInterface* interpreter,
    DYN_CJNativeInterface* runtime, int, INT_InterpreterArgs)
{
    if (runtime->version != DYN_CJNATIVE_INTERFACE_VERSION ||
        interpreter->version != INT_INTERPRETER_INTERFACE_VERSION) { return 1; }
    native = *runtime;
    interpreter->cjThreadOnStart = Start;
    interpreter->cjThreadOnDestroy = Destroy;
    interpreter->iterateFramesWithState = Iterate;
    interpreter->visitFrameRootsMarking = Mark;
    interpreter->visitFrameRootsAdjusting = Adjust;
    interpreter->visitGlobalRoots = Globals;
    return 0;
}
