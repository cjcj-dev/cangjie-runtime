// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <windows.h>
#include <cstring>
#include <type_traits>

#include "Exception.h"
#include "Mutator/Mutator.h"

extern "C" void CJ_MCC_ThrowStackOverflowError();
extern "C" void CJ_MCC_C2NStub();

namespace MapleRuntime {
static_assert(sizeof(FrameAddress) == 16, "Windows native frame anchor");
static_assert(std::is_trivially_destructible<CONTEXT>::value, "context transfer must not skip a destructor");
static_assert(std::is_trivially_destructible<CalleeSavedRegisterContext>::value,
              "context transfer must not skip a destructor");
static_assert(offsetof(CalleeSavedRegisterContext, xmm15) - offsetof(CalleeSavedRegisterContext, xmm6) ==
              9 * sizeof(XMMReg), "managed XMM context array");
static_assert(offsetof(CONTEXT, Xmm15) - offsetof(CONTEXT, Xmm6) == 9 * sizeof(M128A),
              "native XMM context array");
bool IsWindowsExceptionReturnBoundary(const MachineFrame& frame)
{
    if (frame.IsC2RStubFrame() || frame.IsC2NStubFrame() || frame.IsStackGrowStubFrame() ||
        frame.IsExclusiveStubFrame() || frame.IsSafepointHandlerStubFrame() ||
        frame.IsReturnSafepointHandlerStubFrame()) {
        return true;
    }
    DWORD64 image = 0;
    auto* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(frame.GetIP()), &image, nullptr);
    return function != nullptr && image + function->BeginAddress ==
        reinterpret_cast<DWORD64>(&CJ_MCC_ThrowStackOverflowError);
}

// Compute the caller context from real native unwind records. This is shared
// by all Windows return bridges, including C2N's private RET 16 convention.
extern "C" void MRT_UnwindWindowsRuntimeReturn(CONTEXT* captured, FrameAddress* boundary,
                                               uint32_t calleePopBytes)
{
    CHECK_DETAIL(calleePopBytes == 0 || calleePopBytes == 16, "invalid Windows runtime callee cleanup");
    CONTEXT& native = *captured;
    const DWORD64 nativeCallerSp = reinterpret_cast<DWORD64>(boundary) + sizeof(FrameAddress);
    const DWORD64 returnPc = reinterpret_cast<DWORD64>(boundary->returnAddress);
    DWORD64 boundaryStart = 0;
    while (native.Rsp < nativeCallerSp) {
        const DWORD64 previousSp = native.Rsp;
        DWORD64 image = 0;
        auto* function = RtlLookupFunctionEntry(native.Rip, &image, nullptr);
        if (function == nullptr) {
            boundaryStart = 0;
            // The documented Windows leaf-frame rule consumes its real RA.
            native.Rip = *reinterpret_cast<const DWORD64*>(native.Rsp);
            native.Rsp += sizeof(DWORD64);
        } else {
            boundaryStart = image + function->BeginAddress;
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, native.Rip, function, &native,
                             &handlerData, &establisher, nullptr);
        }
        CHECK_DETAIL(native.Rsp > previousSp && native.Rsp <= nativeCallerSp,
                     "Windows exception unwind crossed its native boundary");
    }
    CHECK_DETAIL(native.Rsp == nativeCallerSp && native.Rip == returnPc,
                 "Windows exception unwind did not reach its real caller");
    CHECK_DETAIL(boundaryStart != 0, "Windows exception boundary is missing native unwind records");
    CHECK_DETAIL((boundaryStart == reinterpret_cast<DWORD64>(&CJ_MCC_C2NStub)) == (calleePopBytes == 16),
                 "Windows exception return cleanup does not match its actual boundary");
    // Virtual unwinding a body does not execute C2N's RET 16. Its two private
    // stack arguments belong to the boundary, not to the resumed managed SP.
    native.Rsp += calleePopBytes;
}

extern "C" void MRT_TransferWindowsException(CONTEXT* captured,
                                              const CalleeSavedRegisterContext* restored,
                                              uintptr_t landingPad)
{
    CONTEXT& native = *captured;
    const CalleeSavedRegisterContext& managed = *restored;
    native.Rbx = managed.rbx;
    native.Rsi = managed.rsi;
    native.Rdi = managed.rdi;
    native.R12 = managed.r12;
    native.R13 = managed.r13;
    native.R14 = managed.r14;
    native.R15 = managed.r15;
    native.Rbp = managed.rbp;
    native.Rsp = managed.rsp;
    static_assert(sizeof(XMMReg) == sizeof(M128A), "Windows XMM context layout");
    std::memcpy(&native.Xmm6, &managed.xmm6, 10 * sizeof(XMMReg));
    native.Rip = landingPad;
    native.Rax = 0;
    native.Rdx = 0;
    native.Xmm0 = {};
    native.Xmm1 = {};
    RtlRestoreContext(&native, nullptr);
    LOG(RTLOG_FATAL, "RtlRestoreContext unexpectedly returned");
}

// Called by the actual native boundary, before its epilogue. All intervening
// C++ frames have returned normally; this function owns no RAII resources.
// RtlVirtualUnwind only computes a context, and never substitutes for C++ cleanup.
extern "C" __attribute__((noinline)) void MRT_CompleteWindowsExceptionReturn(FrameAddress* boundary,
                                                                            uint32_t calleePopBytes)
{
    Mutator* mutator = Mutator::GetMutator();
    // Runtime initialization may use a C2R entry before a mutator exists. Such
    // an entry cannot have an ExceptionWrapper with a pending return target.
    if (mutator == nullptr) { return; }
    ExceptionWrapper& wrapper = mutator->GetExceptionWrapper();
    if (wrapper.GetWindowsReturnFrame() != boundary) { return; }
    // The native callee has returned; publish the live bridge before walking
    // or processing roots. Heal the exposed saves before taking their values
    // into the OS snapshot. MRT_RestoreContext still processes each managed
    // frame before consuming its stackmap (ZGC sharedRuntime.cpp:573-636).
    mutator->UpdateUnwindContext();
    StackWatermarkSet::start_processing(*mutator);
    CONTEXT native{};
    RtlCaptureContext(&native);
    MRT_UnwindWindowsRuntimeReturn(&native, boundary, calleePopBytes);

    CalleeSavedRegisterContext managed{};
    managed.rbx = native.Rbx;
    managed.rsi = native.Rsi;
    managed.rdi = native.Rdi;
    managed.r12 = native.R12;
    managed.r13 = native.R13;
    managed.r14 = native.R14;
    managed.r15 = native.R15;
    managed.rbp = native.Rbp;
    managed.rsp = native.Rsp;
    static_assert(sizeof(XMMReg) == sizeof(M128A), "Windows XMM context layout");
    std::memcpy(&managed.xmm6, &native.Xmm6, 10 * sizeof(XMMReg));
    wrapper.SetWindowsReturnFrame(nullptr);
    // Keep the existing ZGC watermark/stackmap restore and TLS refresh as the
    // sole managed-context producer (sharedRuntime.cpp:573-636).
    const uintptr_t landingPad = MRT_RestoreContext(managed);

    MRT_TransferWindowsException(&native, &managed, landingPad);
}
} // namespace MapleRuntime
