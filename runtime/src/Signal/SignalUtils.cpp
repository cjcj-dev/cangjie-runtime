// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SignalUtils.h"

#include <sys/ucontext.h>

#include "securec.h"

namespace MapleRuntime {
// si_code names for machine-readable crash signatures. Always compiled in (not MRT_DEBUG).
// SI_KERNEL(128) is the non-canonical / kernel-fault case that release builds previously
// collapsed into SEGV_UNKNOWN; MAPERR/ACCERR remain the common SEGV codes.
const char* SignalCodeName(int sig, int code)
{
    // SI_USER / SI_KERNEL apply across signals.
    if (code == SI_USER) {
        return "SI_USER";
    }
    if (code == SI_KERNEL) {
        return "SI_KERNEL";
    }
    if (code == SI_QUEUE) {
        return "SI_QUEUE";
    }
    if (code == SI_TIMER) {
        return "SI_TIMER";
    }
    if (code == SI_MESGQ) {
        return "SI_MESGQ";
    }
    if (code == SI_ASYNCIO) {
        return "SI_ASYNCIO";
    }
    if (code == SI_SIGIO) {
        return "SI_SIGIO";
    }
    if (code == SI_TKILL) {
        return "SI_TKILL";
    }

    switch (sig) {
        case SIGSEGV:
            switch (code) {
                case SEGV_MAPERR:
                    return "SEGV_MAPERR";
                case SEGV_ACCERR:
                    return "SEGV_ACCERR";
#ifdef SEGV_BNDERR
                case SEGV_BNDERR:
                    return "SEGV_BNDERR";
#endif
#ifdef SEGV_PKUERR
                case SEGV_PKUERR:
                    return "SEGV_PKUERR";
#endif
                default:
                    return "SEGV_UNKNOWN";
            }
        case SIGBUS:
            switch (code) {
                case BUS_ADRALN:
                    return "BUS_ADRALN";
                case BUS_ADRERR:
                    return "BUS_ADRERR";
                case BUS_OBJERR:
                    return "BUS_OBJERR";
#ifdef BUS_MCEERR_AR
                case BUS_MCEERR_AR:
                    return "BUS_MCEERR_AR";
#endif
#ifdef BUS_MCEERR_AO
                case BUS_MCEERR_AO:
                    return "BUS_MCEERR_AO";
#endif
                default:
                    return "BUS_UNKNOWN";
            }
        case SIGILL:
            switch (code) {
                case ILL_ILLOPC:
                    return "ILL_ILLOPC";
                case ILL_ILLOPN:
                    return "ILL_ILLOPN";
                case ILL_ILLADR:
                    return "ILL_ILLADR";
                case ILL_ILLTRP:
                    return "ILL_ILLTRP";
                case ILL_PRVOPC:
                    return "ILL_PRVOPC";
                case ILL_PRVREG:
                    return "ILL_PRVREG";
                case ILL_COPROC:
                    return "ILL_COPROC";
                case ILL_BADSTK:
                    return "ILL_BADSTK";
                default:
                    return "ILL_UNKNOWN";
            }
        case SIGFPE:
            switch (code) {
                case FPE_INTDIV:
                    return "FPE_INTDIV";
                case FPE_INTOVF:
                    return "FPE_INTOVF";
                case FPE_FLTDIV:
                    return "FPE_FLTDIV";
                case FPE_FLTOVF:
                    return "FPE_FLTOVF";
                case FPE_FLTUND:
                    return "FPE_FLTUND";
                case FPE_FLTRES:
                    return "FPE_FLTRES";
                case FPE_FLTINV:
                    return "FPE_FLTINV";
                case FPE_FLTSUB:
                    return "FPE_FLTSUB";
                default:
                    return "FPE_UNKNOWN";
            }
        case SIGABRT:
            // Kernel-delivered abort typically carries SI_TKILL / SI_KERNEL above.
            return "ABRT_UNKNOWN";
        default:
            return "UNKNOWN";
    }
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
FixedCString PrintSignalInfo(const siginfo_t& info)
{
    constexpr size_t bufferSize = 256;
    char buf[bufferSize];
    int index = 0;
    index += sprintf_s(buf + index, sizeof(buf) - index, "  si_signo: %d (%s)\n  si_code: %d (%s)", info.si_signo,
                       strsignal(info.si_signo), info.si_code, SignalCodeName(info.si_signo, info.si_code));

    CHECK_IN_SIG(index != -1);
    if (info.si_signo == SIGSEGV) {
        CHECK_IN_SIG(sprintf_s(buf + index, sizeof(buf) - index, "\n  si_addr: %p", info.si_addr) != -1);
    }

    return FixedCString(buf);
}
#endif

Uptr GetPCFromUContext(const ucontext_t& ucontext)
{
#if defined(__APPLE__) && defined(__x86_64__)
    return static_cast<Uptr>(ucontext.uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__)
    return static_cast<Uptr>(ucontext.uc_mcontext->__ss.__pc);
#elif defined(__aarch64__)
    return static_cast<Uptr>(ucontext.uc_mcontext.pc);
#elif defined(__arm__)
    return static_cast<Uptr>(ucontext.uc_mcontext.arm_pc);
#elif defined(__x86_64__)
    return static_cast<Uptr>(ucontext.uc_mcontext.gregs[REG_RIP]);
#else // __x86?
    return static_cast<Uptr>(ucontext.uc_mcontext.gregs[REG_EIP]);
#endif
}

Uptr GetFAFromUContext(const ucontext_t& ucontext)
{
#if defined(__APPLE__) && defined(__x86_64__)
    return static_cast<Uptr>(ucontext.uc_mcontext->__ss.__rbp);
#elif defined(__APPLE__) && defined(__aarch64__)
    return static_cast<Uptr>(ucontext.uc_mcontext->__ss.__fp);
#elif defined(__aarch64__)
    constexpr uint32_t fp = 29; // x29 is fp register.
    return static_cast<Uptr>(ucontext.uc_mcontext.regs[fp]);
#elif defined(__arm__)
    return static_cast<Uptr>(ucontext.uc_mcontext.arm_fp);
#elif defined(__x86_64__)
    return static_cast<Uptr>(ucontext.uc_mcontext.gregs[REG_RBP]);
#else // __x86?
    return static_cast<Uptr>(ucontext.uc_mcontext.gregs[REG_EBP]);
#endif
}

namespace {
int AppendHexReg(char* buf, size_t bufSize, int pos, const char* name, unsigned long val)
{
    if (pos < 0 || static_cast<size_t>(pos) >= bufSize) {
        return -1;
    }
    int n = sprintf_s(buf + pos, bufSize - static_cast<size_t>(pos), "%s%s=0x%lx",
                      pos == 0 ? "" : ",", name, val);
    if (n < 0) {
        return -1;
    }
    return pos + n;
}
} // namespace

int FormatRegsFromUContext(const ucontext_t& ucontext, char* buf, size_t bufSize)
{
    if (buf == nullptr || bufSize == 0) {
        return 0;
    }
    buf[0] = '\0';
    int pos = 0;
#if defined(__x86_64__) && !defined(__APPLE__)
    pos = AppendHexReg(buf, bufSize, pos, "rax", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RAX]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rbx", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RBX]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rcx", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RCX]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rdx", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RDX]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rsi", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RSI]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rdi", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RDI]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rbp", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RBP]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rsp", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RSP]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r8", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R8]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r9", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R9]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r10", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R10]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r11", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R11]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r12", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R12]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r13", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R13]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r14", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R14]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "r15", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_R15]));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "rip", static_cast<unsigned long>(ucontext.uc_mcontext.gregs[REG_RIP]));
    if (pos < 0) {
        return 0;
    }
#elif defined(__aarch64__) && !defined(__APPLE__)
    for (int i = 0; i < 31; ++i) {
        char name[8];
        if (sprintf_s(name, sizeof(name), "x%d", i) < 0) {
            return 0;
        }
        pos = AppendHexReg(buf, bufSize, pos, name,
                           static_cast<unsigned long>(ucontext.uc_mcontext.regs[i]));
        if (pos < 0) {
            return 0;
        }
    }
    pos = AppendHexReg(buf, bufSize, pos, "sp", static_cast<unsigned long>(ucontext.uc_mcontext.sp));
    if (pos < 0) {
        return 0;
    }
    pos = AppendHexReg(buf, bufSize, pos, "pc", static_cast<unsigned long>(ucontext.uc_mcontext.pc));
    if (pos < 0) {
        return 0;
    }
#else
    (void)ucontext;
    (void)AppendHexReg;
    int n = sprintf_s(buf, bufSize, "unsupported");
    return n > 0 ? n : 0;
#endif
    return pos > 0 ? pos : 0;
}
} // namespace MapleRuntime
