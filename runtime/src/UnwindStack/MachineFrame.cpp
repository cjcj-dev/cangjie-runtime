// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/MemUtils.h"
#include "Common/StackType.h"
#include "Interpreter/InterpreterSpecific.h"
#include "StackManager.h"
#include "securec.h"
#ifdef _WIN64
#include "Common/Runtime.h"
#include "UnwindWin.h"
#endif

extern uintptr_t unwindPCForSafepointHandlerStub;
#if defined(__x86_64__) || defined(__aarch64__)
extern uintptr_t unwindPCForReturnSafepointHandlerStub;
#endif
extern uintptr_t unwindPCForN2CStub;
extern uintptr_t unwindPCForC2NStub;
extern uintptr_t unwindPCForC2RStubStart;
extern uintptr_t unwindPCForC2RStubEnd;
extern uintptr_t unwindPCForStackGrowStub;
extern uintptr_t unwindPCForExclusiveStubFull;
extern uintptr_t unwindPCForExclusiveStub;
#ifdef _WIN64
// Bounds describe the fully established save area, not the whole function.
// A CALL at the end returns to BodyEnd before the first restore instruction.
// Keep that real return PC for SEH; classification must never rewrite it.
#define WINDOWS_STUB_BODY(name) \
    extern uintptr_t windows##name##BodyStart; \
    extern uintptr_t windows##name##BodyEnd;
WINDOWS_STUB_BODY(C2N)
WINDOWS_STUB_BODY(StackGrow)
WINDOWS_STUB_BODY(Safepoint)
WINDOWS_STUB_BODY(ReturnSafepoint)
WINDOWS_STUB_BODY(Exclusive)
WINDOWS_STUB_BODY(StackOverflow)
#undef WINDOWS_STUB_BODY
extern uintptr_t windowsC2NFailureStart;
extern uintptr_t windowsC2NFailureEnd;
#endif
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
extern uintptr_t unwindPCForRuntimeStubStart;
extern uintptr_t unwindPCForRuntimeStubEnd;
extern uintptr_t unwindPCForC2NStubExceptionReturn;
extern uintptr_t CalleeSavedStubFrameSize;
extern uintptr_t C2NStubFrameSize;
#endif
namespace MapleRuntime {
#ifdef _WIN64
static bool IsWindowsStubBody(const uint32_t* pc, const uintptr_t* begin, const uintptr_t* end)
{
    const auto address = reinterpret_cast<uintptr_t>(pc);
    return address > reinterpret_cast<uintptr_t>(begin) && address <= reinterpret_cast<uintptr_t>(end);
}
#endif
bool MachineFrame::IsN2CStubFrame() const
{
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    uintptr_t strippedIP = PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip));
    return strippedIP == reinterpret_cast<uintptr_t>(&unwindPCForN2CStub) ||
        strippedIP == reinterpret_cast<uintptr_t>(&unwindPCForExclusiveStubFull);
#else
    uintptr_t ipAddr = reinterpret_cast<uintptr_t>(ip);
    return ipAddr == reinterpret_cast<uintptr_t>(&unwindPCForN2CStub) ||
        ipAddr == reinterpret_cast<uintptr_t>(&unwindPCForExclusiveStubFull);
#endif
}

bool MachineFrame::IsExclusiveStubFrame() const
{
#ifdef _WIN64
    return IsWindowsStubBody(ip, &windowsExclusiveBodyStart, &windowsExclusiveBodyEnd);
#elif defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) ==
        reinterpret_cast<uintptr_t>(&unwindPCForExclusiveStub);
#else
    return reinterpret_cast<uintptr_t>(ip) == reinterpret_cast<uintptr_t>(&unwindPCForExclusiveStub);
#endif
}

bool MachineFrame::IsRuntimeFrame() const
{
    return StackManager::IsRuntimeFrame(reinterpret_cast<uintptr_t>(ip));
}

#ifdef INTERPRETER_ENABLED
bool MachineFrame::IsInterpreterFrame() const
{
    return StackManager::IsInterpreterCodeAddr(reinterpret_cast<uintptr_t>(ip));
}

bool MachineFrame::IsC2IStubFrame() const
{
    return IsC2IStubAddr(reinterpret_cast<uintptr_t>(ip));
}

bool MachineFrame::IsI2IFrame() const
{
    return IsI2IAdapterStartAddr(reinterpret_cast<uintptr_t>(ip));
}

bool MachineFrame::IsI2NFrame() const
{
    return IsI2NStubAddr(reinterpret_cast<uintptr_t>(ip));
}
#endif

bool MachineFrame::IsC2NStubFrame() const
{
#ifdef _WIN64
    return IsWindowsStubBody(ip, &windowsC2NBodyStart, &windowsC2NBodyEnd) ||
        IsWindowsStubBody(ip, &windowsC2NFailureStart, &windowsC2NFailureEnd);
#elif defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) == reinterpret_cast<uintptr_t>(&unwindPCForC2NStub);
#else
    return reinterpret_cast<uintptr_t>(ip) == reinterpret_cast<uintptr_t>(&unwindPCForC2NStub);
#endif
}

bool MachineFrame::IsStackGrowStubFrame() const
{
#ifdef _WIN64
    return IsWindowsStubBody(ip, &windowsStackGrowBodyStart, &windowsStackGrowBodyEnd);
#elif defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) ==
        reinterpret_cast<uintptr_t>(&unwindPCForStackGrowStub);
#else
    return reinterpret_cast<uintptr_t>(ip) == reinterpret_cast<uintptr_t>(&unwindPCForStackGrowStub);
#endif
}

bool MachineFrame::IsC2RStubFrame() const
{
#ifdef _WIN64
    // The stack-overflow entry has the same seven-register save layout as
    // CalleeSavedRegistersStub; it also returns through the native bridge.
    if (IsWindowsStubBody(ip, &windowsStackOverflowBodyStart, &windowsStackOverflowBodyEnd)) { return true; }
#endif
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) >
        reinterpret_cast<uintptr_t>(&unwindPCForC2RStubStart) &&
        PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) < reinterpret_cast<uintptr_t>(&unwindPCForC2RStubEnd);
#else
    return reinterpret_cast<uintptr_t>(ip) > reinterpret_cast<uintptr_t>(&unwindPCForC2RStubStart) &&
        reinterpret_cast<uintptr_t>(ip) < reinterpret_cast<uintptr_t>(&unwindPCForC2RStubEnd);
#endif
}

#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
bool MachineFrame::IsC2NExceptionStubFrame() const
{
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) ==
        reinterpret_cast<uintptr_t>(&unwindPCForC2NStubExceptionReturn);
}
#endif

bool MachineFrame::IsSafepointHandlerStubFrame() const
{
#ifdef _WIN64
    return IsWindowsStubBody(ip, &windowsSafepointBodyStart, &windowsSafepointBodyEnd);
#elif defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    return PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip)) ==
        reinterpret_cast<uintptr_t>(&unwindPCForSafepointHandlerStub);
#else
    return reinterpret_cast<uintptr_t>(ip) == reinterpret_cast<uintptr_t>(&unwindPCForSafepointHandlerStub);
#endif
}

bool MachineFrame::IsReturnSafepointHandlerStubFrame() const
{
#ifdef _WIN64
    return IsWindowsStubBody(ip, &windowsReturnSafepointBodyStart, &windowsReturnSafepointBodyEnd);
#elif defined(__x86_64__) || defined(__aarch64__)
    return reinterpret_cast<uintptr_t>(ip) == reinterpret_cast<uintptr_t>(&unwindPCForReturnSafepointHandlerStub);
#else
    return false;
#endif
}

bool MachineFrame::IsAnchorFrame(const uint32_t* anchorFA) const
{
    return reinterpret_cast<uintptr_t>(anchorFA) == reinterpret_cast<uintptr_t>(fa) || ip == nullptr;
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
CString MachineFrame::DumpMachineFrame() const
{
    static constexpr size_t defaultBufSize = 128;
    char buf[defaultBufSize];
    if (snprintf_s(buf, sizeof buf, sizeof buf - 1, ": frame pc %p, frame address %p", ip, fa) < 0) {
        LOG(RTLOG_ERROR, "Dump snprintf_s fail");
    }
    return CString(buf);
}

void MachineFrame::Dump(const CString msg) const { LOG(RTLOG_INFO, "%s %s", msg.Str(), DumpMachineFrame().Str()); }
#endif

#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
uint64_t GetRuntimeFrameSize(MachineFrame &mFrame)
{
    uint64_t size = 0;
    if (mFrame.IsC2RStubFrame()) {
        size = CalleeSavedStubFrameSize;
    } else if (mFrame.IsC2NExceptionStubFrame()) {
        size = C2NStubFrameSize;
    } else {
        LOG(RTLOG_FATAL, "invalid frame, abort.");
    }
    return size;
}
#endif

// return SUCC or FINISH only, do not return FAIL.
// caller assures this frame is a normal frame.
// we name the direct caller frame in machine stack with "machine caller".
#ifdef _WIN64
bool MachineFrame::UnwindToCallerMachineFrame(FrameInfo& caller, UnwindContextStatus& status) const
{
    Runtime& runtime = Runtime::Current();
    WinModuleManager& winModuleManager = runtime.GetWinModuleManager();
    caller = GetCallerFrameInfo(winModuleManager, *this, status);

    return true;
}
#else

bool MachineFrame::UnwindToCallerMachineFrame(MachineFrame& caller) const
{
    FrameAddress* curFp = this->fa; // backup current frame address
    FrameAddress* callerFp = curFp->callerFrameAddress;
    caller.fa = callerFp;
#if defined(__OHOS__) && defined(__aarch64__) && !defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    caller.ip = PtrauthStripInstPointer(curFp->returnAddress);
#else
    caller.ip = curFp->returnAddress;
#endif
    return true;
}
#endif
} // namespace MapleRuntime
