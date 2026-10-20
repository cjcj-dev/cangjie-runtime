// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "UnwindCApi.h"
#include "Mutator/VMExit.h"

#include "schedule.h"
#include "Exception/ExceptionCApi.h"
#include "Mutator/Mutator.inline.h"
#ifdef _WIN64
#include "UnwindWin.h"
#endif
#if defined(__x86_64__)
extern uintptr_t unwindPCForC2NStub;
#endif

namespace MapleRuntime {
// The stub frame is about to return. Its caller stays on the stack. Clearing
// both ends here is GoIntoManagedCode, which is not ZGC's !has_last_Java_frame().
static void PublishLiveCallerOrClear(UnwindContext& uwContext)
{
    MachineFrame& top = uwContext.frameInfo.mFrame;
    const bool hasFA = top.GetFA() != nullptr;
    const bool hasIP = top.GetIP() != nullptr;
    CHECK_DETAIL(hasFA == hasIP, "unwind top is missing only the frame address or only the instruction");
    FrameAddress* fa = top.GetFA();
    if (fa != nullptr && fa->callerFrameAddress != nullptr && fa->returnAddress != nullptr) {
        top.SetFA(fa->callerFrameAddress);
        top.SetIP(fa->returnAddress);
        uwContext.SetUnwindContextStatus(UnwindContextStatus::RELIABLE);
        return;
    }
    uwContext.GoIntoManagedCode();
}

extern "C" void MRT_RestoreTopManagedContextFromN2CStub(FrameAddress* fa)
{
    UnwindContext& uwContext = Mutator::GetMutator()->GetUnwindContext();
    N2CSlotData* slotData = N2CFrame::GetSlotData(fa);
    const bool hasFA = slotData->fa != nullptr;
    const bool hasIP = slotData->pc != nullptr;
    CHECK_DETAIL(hasFA == hasIP, "unwind top is missing only the frame address or only the instruction");
    uwContext.frameInfo.mFrame.SetIP(slotData->pc);
    uwContext.frameInfo.mFrame.SetFA(slotData->fa);
    uwContext.SetUnwindContextStatus(slotData->status);
}

extern "C" void MRT_SaveTopManagedContextToN2CStub(FrameAddress* fa)
{
    UnwindContext& uwContext = Mutator::GetMutator()->GetUnwindContext();
    N2CSlotData* slotData = N2CFrame::GetSlotData(fa);
    const bool hasFA = uwContext.frameInfo.mFrame.GetFA() != nullptr;
    const bool hasIP = uwContext.frameInfo.mFrame.GetIP() != nullptr;
    CHECK_DETAIL(hasFA == hasIP, "unwind top is missing only the frame address or only the instruction");
    slotData->pc = uwContext.frameInfo.mFrame.GetIP();
    slotData->fa = uwContext.frameInfo.mFrame.GetFA();
    // By default, all caller functions that pass through N2CStub are risky types
    // and need to use the data on the slot to unwind.
    slotData->status = uwContext.GetUnwindContextStatus();
    uwContext.GoIntoManagedCode();
}

extern "C" void MRT_SaveC2NContext(const uint32_t* pc, void* fa, ThreadLocalData* tlData)
{
    UnwindContext& uwContext = tlData->mutator->GetUnwindContext();
    uwContext.GoIntoNativeCode(pc, fa);
}

extern "C" void MRT_DeleteC2NContext(ThreadLocalData* tlData)
{
    if (UNLIKELY(tlData == nullptr)) {
        return;
    }
    Mutator* mutator = tlData->mutator;
    if (UNLIKELY(mutator == nullptr)) {
        return;
    }
    PublishLiveCallerOrClear(mutator->GetUnwindContext());
}

extern "C" void MRT_UpdateUwContext(const uint32_t* pc, void* fa, UnwindContextStatus status, ThreadLocalData* tlData)
{
    CHECK_DETAIL(pc != nullptr && fa != nullptr,
        "published unwind top requires both a frame address and an instruction");
    UnwindContext& uwContext = tlData->mutator->GetUnwindContext();
    uwContext.frameInfo.mFrame.SetIP(pc);
    uwContext.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(fa));
    uwContext.SetUnwindContextStatus(status);
}

extern "C" bool MRT_C2N_Enter(const uint32_t* pc, void* fa, ThreadLocalData* tlData)
{
    auto *mutator = tlData->mutator;
    // Step 1: SaveC2NContext - set PC, FA, RISKY status
    UnwindContext& uwContext = mutator->GetUnwindContext();
    uwContext.GoIntoNativeCode(pc, fa);

    // Step 2: EnterSaferegion(updateUnwindContext=false)
    // Since context is already set in GoIntoNativeCode, pass false here
    return mutator->EnterSaferegion(false);
}

extern "C" ThreadLocalData* MRT_C2N_Leave(bool safeState, unsigned long long stackTop)
{
    VM_Exit::wait_if_vm_exited();
#ifdef __OHOS__
    if (stackTop != 0) {
        PopUIThreadStackTop();
    }
#else
    (void)stackTop;
#endif
    // Get ThreadLocalData internally
    ThreadLocalData* tlData = reinterpret_cast<ThreadLocalData*>(MRT_GetThreadLocalData());
    if (UNLIKELY(tlData == nullptr)) {
        return nullptr;
    }

    auto* mutator = tlData->mutator;
    if (UNLIKELY(mutator == nullptr)) {
        return tlData;
    }

    if (safeState) {
        // Step 1: LeaveSaferegion - includes suspension request check
        MRT_LeaveNative();
    } else {
        ProcessSafepointIfRequested(tlData);
    }

    // Step 2: the native frame is gone. Keep its still-live caller.
    PublishLiveCallerOrClear(mutator->GetUnwindContext());

    // Step 3: ThrowPendingException only on exception path to avoid an extra PLT call in common case.
    ExceptionWrapper& exceptionWrapper = mutator->GetExceptionWrapper();
    if (UNLIKELY(exceptionWrapper.GetExceptionRef() != nullptr)) {
        MRT_ThrowPendingException(nullptr);
    }

    return tlData;
}

#ifdef __OHOS__
extern "C" MRT_EXPORT unsigned long long MRT_UpdateStackTop(unsigned long long stackTop)
{
    // Step1: GetUIThreadStackTop - include runtime finish checking
    auto stack = GetUIThreadStackTop();
    if (stack == 0) {
        return 0;
    }

    // Step2: PushUIThreadStackTop - no need re-check runtime finish
    PushUIThreadStackTop(stackTop);
    return stack;
}
#endif

extern "C" void MRT_PreRunManagedCode(Mutator* mutator, int layers, ThreadLocalData* threadData)
{
    if (UNLIKELY(MutatorManager::Instance().SyncTriggered())) {
        mutator->SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_SYNC);
        mutator->EnterSaferegion(false);
    }
    mutator->SetManagedContext(true);
    mutator->LeaveSaferegion();
#ifdef _WIN64
    Runtime& runtime = Runtime::Current();
    WinModuleManager& winModuleManager = runtime.GetWinModuleManager();
    Uptr rip = 0;
    Uptr rsp = 0;
    GetContextWin64(&rip, &rsp);
    FrameInfo curFrame = GetCurFrameInfo(winModuleManager, rip, rsp);
    UnwindContextStatus ucs = UnwindContextStatus::UNKNOWN;
    FrameInfo callerFrame = GetCallerFrameInfo(winModuleManager, curFrame.mFrame, ucs);

    UnwindContext& uwContext = Mutator::GetMutator()->GetUnwindContext();
    uwContext.anchorFA = reinterpret_cast<uint32_t*>(callerFrame.mFrame.GetFA());

    for (int i = 0; i < layers; ++i) {
        callerFrame = GetCallerFrameInfo(winModuleManager, callerFrame.mFrame, ucs);
        uwContext.anchorFA = reinterpret_cast<uint32_t*>(callerFrame.mFrame.GetFA());
    }
#else
    UnwindContext& uwContext = mutator->GetUnwindContext();
    // current function fa
    void* fa = __builtin_frame_address(0);
    uwContext.anchorFA = reinterpret_cast<uint32_t*>(reinterpret_cast<FrameAddress*>(fa)->callerFrameAddress);

    for (int i = 0; i < layers; ++i) {
        uwContext.anchorFA =
            reinterpret_cast<uint32_t*>(reinterpret_cast<FrameAddress*>(uwContext.anchorFA)->callerFrameAddress);
    }
#endif
    uwContext.GoIntoManagedCode();
    mutator->InitStackInfo(threadData);
}
} // namespace MapleRuntime
