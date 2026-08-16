// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SignalManager.h"

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>

#include "Base/GcLog.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "LoaderManager.h"
// paramzero: avoid #include WCollector.h (its Heap include graph needs WCollector TU paths).
namespace MapleRuntime {
void EmitParamzeroCrashProbe(uintptr_t rbp, uintptr_t rbx, uintptr_t rip);
}
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Signal/SignalUtils.h"
#include "Inspector/CjHeapData.h"
#include "Heap/Collector/TaskQueue.h"
#include "Heap/Verify/StartWhoDiag.h"
#include "Heap/Verify/WhoPushDiag.h"
#include "Heap/Verify/HeldFreeDiag.h"
#include "Heap/Verify/TlRawDiag.h"
#include "Heap/Verify/HealPairDiag.h"
#include "Heap/Verify/MarkFaceSnap.h"
#include "Heap/Verify/HdrWhoDiag.h"
#include "securec.h"
#ifdef COV_SIGNALHANDLE
extern "C" void __gcov_dump(void);
#endif
namespace MapleRuntime {

namespace {
// AS-safe stderr write for signal-handler diagnostic path only.
void WriteSigDiag(const char* buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    (void)write(STDERR_FILENO, buf, len);
}

// FLOG-compatible ERROR line without logMutex: "<tid> E <msg>\n"
void LogErrorAsSafe(const char* msg)
{
    char buf[512];
    int n = sprintf_s(buf, sizeof(buf), "%d E %s\n", static_cast<int>(GetTid()), msg);
    if (n > 0) {
        WriteSigDiag(buf, static_cast<size_t>(n));
    }
}

// Fold free-text phase names into one key=value token (same rule as GcLog::Phase).
void FoldToken(const char* in, char* out, size_t outCap)
{
    if (out == nullptr || outCap == 0) {
        return;
    }
    if (in == nullptr) {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < outCap && in[i] != '\0'; ++i) {
        char c = in[i];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
        out[i] = keep ? c : '_';
    }
    out[i] = '\0';
}

// Basename of a path without allocating (scan from the end).
const char* PathBase(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return "unknown";
    }
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base[0] == '\0' ? "unknown" : base;
}

// Probe-read up to nBytes at pc into out hex string "aabbcc...". On failure writes "unreadable".
// Only attempted when dladdr already resolved the PC (page is likely mapped). No heap, no lock.
void FormatInsnHex(uintptr_t pc, char* out, size_t outCap, size_t nBytes)
{
    if (out == nullptr || outCap < 12) {
        return;
    }
    if (pc == 0 || nBytes == 0) {
        (void)sprintf_s(out, outCap, "none");
        return;
    }
    // Cap to what fits as hex in outCap (2 chars/byte + NUL).
    size_t maxBytes = (outCap - 1) / 2;
    if (nBytes > maxBytes) {
        nBytes = maxBytes;
    }
    if (nBytes > 16) {
        nBytes = 16;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(pc);
    // Best-effort: if the page was unmapped this may re-fault. Callers only invoke after
    // dladdr success so the text mapping is typically still present. Nested SEGV is
    // accepted as "diagnostic path re-fault" rather than inventing a non-AS-safe probe.
    size_t pos = 0;
    for (size_t i = 0; i < nBytes; ++i) {
        int n = sprintf_s(out + pos, outCap - pos, "%02x", static_cast<unsigned>(p[i]));
        if (n < 0) {
            (void)sprintf_s(out, outCap, "unreadable");
            return;
        }
        pos += static_cast<size_t>(n);
    }
    out[pos] = '\0';
}

// Emit machine-readable crash signature (GcLog schema v=3 rec=crash). Independent of
// MRT_GC_LOG so a crash before GcLog init still emits. Uses only stack + write(2).
void EmitCrashRec(int sig, const siginfo_t* info, void* context, uintptr_t sigPc, uintptr_t sigRbp)
{
    int siCode = (info != nullptr) ? info->si_code : 0;
    const void* siAddr = (info != nullptr) ? info->si_addr : nullptr;
    const char* sigName = SignalManager::GetSignalName(static_cast<uint8_t>(sig));
    const char* codeName = SignalCodeName(sig, siCode);

    // pc_mod + pc_off: dladdr is the existing runtime pattern (StackManager / Loader).
    // Not strictly POSIX AS-safe, but matches the in-tree precedent and is the only way
    // to get a stable cross-run offset without a private module table.
    const char* pcMod = "unknown";
    unsigned long pcOff = 0;
    const char* sym = "none";
    bool pcResolved = false;
    if (sigPc != 0) {
        Dl_info dli {};
        if (dladdr(reinterpret_cast<void*>(sigPc), &dli) != 0 && dli.dli_fbase != nullptr) {
            pcResolved = true;
            pcMod = PathBase(dli.dli_fname);
            pcOff = static_cast<unsigned long>(sigPc - reinterpret_cast<uintptr_t>(dli.dli_fbase));
            if (dli.dli_sname != nullptr) {
                sym = dli.dli_sname;
            }
        }
    }

    char regsBuf[768];
    regsBuf[0] = '\0';
    if (context != nullptr) {
        (void)FormatRegsFromUContext(*static_cast<ucontext_t*>(context), regsBuf, sizeof(regsBuf));
    }
    if (regsBuf[0] == '\0') {
        (void)sprintf_s(regsBuf, sizeof(regsBuf), "none");
    }

    char insnBuf[40];
    if (pcResolved) {
        FormatInsnHex(sigPc, insnBuf, sizeof(insnBuf), 16);
    } else {
        (void)sprintf_s(insnBuf, sizeof(insnBuf), "unreadable");
    }

    // GC phase: only if Runtime/Heap already exist. Crash can happen before init.
    char phaseTok[48] = "none";
    const char* gcKind = "none";
    int inParFix = 0;
    uint64_t seq = 0;
    if (Runtime::CurrentRef() != nullptr) {
        seq = GcLog::CurrentSeq();
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        FoldToken(Collector::GetGCPhaseName(phase), phaseTok, sizeof(phaseTok));
        if (phase == GC_PHASE_PREFORWARD || phase == GC_PHASE_FORWARD) {
            inParFix = 1;
            gcKind = "fix";
        } else if (phase == GC_PHASE_IDLE || phase == GC_PHASE_UNDEF) {
            gcKind = "none";
        } else {
            gcKind = "active";
        }
    }

    char assertBuf[GcLog::FATAL_SLOT_CAP];
    size_t assertLen = GcLog::CopyFatal(assertBuf, sizeof(assertBuf));
    const char* assertTok = assertLen > 0 ? assertBuf : "none";

    // One line, stable field order. Schema v=3 advances only for rec=crash.
    char line[2048];
    int n = sprintf_s(line, sizeof(line),
                      "[GCLOG] v=%u rec=crash seq=%llu signo=%d signame=%s si_code=%d si_codename=%s "
                      "si_addr=%p pc=0x%lx pc_mod=%s pc_off=0x%lx sym=%s rbp=0x%lx "
                      "gc_phase=%s gc_kind=%s in_par_fix=%d regs=%s insn=%s assert=%s\n",
                      GcLog::CRASH_SCHEMA_VERSION, static_cast<unsigned long long>(seq), sig, sigName, siCode,
                      codeName, siAddr, static_cast<unsigned long>(sigPc), pcMod, pcOff, sym,
                      static_cast<unsigned long>(sigRbp), phaseTok, gcKind, inParFix, regsBuf, insnBuf, assertTok);
    if (n > 0) {
        WriteSigDiag(line, static_cast<size_t>(n));
    }
    // paramzero: dump -0x50(%rbp) + heap CAS-null counters (gate MRT_GCV2_NULLSLOT).
    // Mode A pc_off=0x6ef90a / si_addr=0x38: answer "was entry arg already 0?".
    if (context != nullptr) {
        const ucontext_t& uctx = *static_cast<const ucontext_t*>(context);
#if defined(__x86_64__) && !defined(__APPLE__)
        uintptr_t rbx = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RBX]);
        uintptr_t rdi = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RDI]);
        uintptr_t rax = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RAX]);
        uintptr_t r12 = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_R12]);
        uintptr_t r14 = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_R14]);
        uintptr_t rbp = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RBP]);
        uintptr_t rcx = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RCX]);
        uintptr_t rdx = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RDX]);
        uintptr_t rsi = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_RSI]);
        uintptr_t r13 = static_cast<uintptr_t>(uctx.uc_mcontext.gregs[REG_R13]);
        // holdercapture runs FIRST, deliberately.
        //
        // This handler is a sequence of probes with no isolation between them: the first
        // one that faults or trips a fatal check takes every probe after it down. That is
        // not hypothetical — MAIN n10 died here with
        //   "F GetUnitIdxAt OOB addr=... ra1=NoTracedDiag::NoteCrashJoin
        //    ra2=HealPairDiag::NoteCrashWhoZero"
        // on a SIGSEGV whose r13 was not a heap address, and everything downstream of
        // NoteCrashWhoZero was simply never reached. Ordering is the only isolation
        // available here, so the reading this lane exists to take goes before the probes
        // that are known to abort.
        //
        // The question it asks is also weaker than the others on purpose: not "does this
        // look like a known crash signature" but "was this address inside a region we
        // freed, and what did that region's mark face say at the moment of the free".
        {
            const uintptr_t sweepAddrs[] = { reinterpret_cast<uintptr_t>(siAddr), rdi, rsi, rdx,
                                             rcx, rax, rbx, r12, r13, r14, rbp };
            const char* const sweepNames[] = { "si_addr", "rdi", "rsi", "rdx", "rcx", "rax",
                                               "rbx", "r12", "r13", "r14", "rbp" };
            MarkFaceSnap::NoteCrashSweep(sweepAddrs, sweepNames,
                                         sizeof(sweepAddrs) / sizeof(sweepAddrs[0]));
        }
#else
        uintptr_t rbx = 0;
#endif
        EmitParamzeroCrashProbe(sigRbp, rbx, sigPc);
#if defined(__x86_64__) && !defined(__APPLE__)
        TlRawDiag::NoteCrashRdi(rdi);
        StartWhoDiag::NoteCrash();
        WhoPushDiag::NoteCrashRdi(rdi);
        HealPairDiag::NoteCrashRdi(rdi);
        HdrWhoDiag::NoteCrashRdi(rdi);
        HealPairDiag::NoteCrashRegs(rdi, rax, r12, r14, rbp);
        HeldFreeDiag::NoteCrashRegs(rax, rbx, rcx, rdx, rsi, rdi, r12, r14, rbp);
        HealPairDiag::NoteCrashWhoZero(r13, rcx, rsi, rbx, r12);
#endif
    }
}
} // namespace

void SignalManager::Init()
{
    PrepareSigStack();
    // Block some ignored signals
    BlockSignals();
#if !defined(__OHOS__) && !defined(__ANDROID__) && !defined(__IOS__)
    // Install unexpected handler first
    InstallUnexpectedSignalHandlers();
    // Install sigsegv handler
    InstallSegvHandler();
    // Install sigusr1 handler
    InstallSIGUSR1Handlers();
#endif
#ifdef __OHOS__
    // Install sigusr2 handler
    InstallSIGUSR2Handlers();
#endif
}

void SignalManager::Fini()
{
    FreeSigStack();
}

void SignalManager::PrepareSigStack()
{
    constexpr int stackSizeMultiples = 4;
    signalStack.ss_sp = malloc(SIGSTKSZ * stackSizeMultiples);
    if (signalStack.ss_sp == nullptr) {
        LOG(RTLOG_FATAL, "Alloca signal stack failed.");
    }

    signalStack.ss_size = SIGSTKSZ * stackSizeMultiples;
    signalStack.ss_flags = 0;

    if (sigaltstack(&signalStack, nullptr) == -1) {
        LOG(RTLOG_FATAL, "sigaltstack failed.");
    }
}

void SignalManager::FreeSigStack()
{
    free(signalStack.ss_sp);
}

void SignalManager::BlockSignals()
{
    sigset_t set;
    CHECK_SIGNAL_CALL(sigemptyset, (&set), "sigemptyset failed in BlockSignals");
    CHECK_SIGNAL_CALL(sigaddset, (&set, SIGPIPE), "sigaddset failed in BlockSignals");
    CHECK_SIGNAL_CALL(pthread_sigmask, (SIG_BLOCK, &set, nullptr), "pthread_sigmask failed in BlockSignals");
}

static void CheckStackOverflow(const siginfo_t& info)
{
    if (Runtime::CurrentRef() != nullptr && !Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
        return;
    }
    uintptr_t stackAddr = reinterpret_cast<uintptr_t>(CJThreadStackAddrGet());
    uintptr_t topAddr = stackAddr - MapleRuntime::MRT_PAGE_SIZE;
    uintptr_t sigAddr = reinterpret_cast<uintptr_t>(info.si_addr);
    if (stackAddr != 0 && sigAddr >= topAddr && sigAddr < stackAddr) {
        LogErrorAsSafe("unhandled SIGSEGV from unmanaged stack overflow!");
    }
}

static void CheckSuspendState()
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    Mutator* mutator = tlData->mutator;
    if (mutator == nullptr) {
        return;
    }
    if (mutator->HasSuspensionRequest(Mutator::SuspensionType::SUSPENSION_FOR_EXIT)) {
        while (true) {
            sleep(INT_MAX);
        }
    }
}

void PrintSignalHandlerStack(int sig, const siginfo_t* info, void* context)
{
    // AS-safe path: key fields via stack buffer + write(2).
    // Full unwind / symbolize / FLOG / pthread_getname_np are deferred out of the
    // signal-context critical path (REPORT-gchang11 §5 D).
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
    PrintUntagRefFieldBreadcrumb();
#endif
    // Emit once per OS signal delivery (HandlerImpl entry) so a user-registered
    // crash handler on an _exit path cannot suppress the pc line.
    uintptr_t sigPc = 0;
    uintptr_t sigRbp = 0;
    if (context != nullptr) {
        ucontext_t* ucontext = static_cast<ucontext_t*>(context);
        sigPc = GetPCFromUContext(*ucontext);
        // GetFAFromUContext returns the frame pointer (RBP/FP), not the fault address.
        // Field renamed to rbp= for clarity; value and source unchanged.
        sigRbp = GetFAFromUContext(*ucontext);
    }
    const void* siAddr = (info != nullptr) ? info->si_addr : nullptr;

    // Compatibility: keep the legacy free-text line byte-stable for existing greps
    // (field key still `fa=` so parsers that match the literal string keep working).
    // A new machine-readable [GCLOG] rec=crash line is emitted next to it for one cycle.
    char line[320];
    int n = sprintf_s(line, sizeof(line),
                      "%d E signal %s (%d) pc=0x%lx fa=0x%lx si_addr=%p\n",
                      static_cast<int>(GetTid()), SignalManager::GetSignalName(static_cast<uint8_t>(sig)), sig,
                      static_cast<unsigned long>(sigPc), static_cast<unsigned long>(sigRbp), siAddr);
    if (n > 0) {
        WriteSigDiag(line, static_cast<size_t>(n));
    }
    EmitCrashRec(sig, info, context, sigPc, sigRbp);
}

bool SignalManager::HandleUnexpectedSignal(int sig, siginfo_t* info, void* context)
{
    CheckSuspendState();
    // pc/fa/si_addr already emitted at HandlerImpl entry (before user handlers).
#ifdef COV_SIGNALHANDLE
    __gcov_dump();
#endif

    return false;
}

void SignalManager::InstallUnexpectedSignalHandlers()
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSignal;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;

    AddHandlerToSignalStack(SIGABRT, &sa);
#ifdef __APPLE__
    AddHandlerToSignalStack(SIGSEGV, &sa);
#else
    AddHandlerToSignalStack(SIGBUS, &sa);
#endif
    AddHandlerToSignalStack(SIGILL, &sa);
    AddHandlerToSignalStack(SIGFPE, &sa);
}

void SignalManager::InstallSIGUSR1Handlers() const
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSIGUSR1;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;
    AddHandlerToSignalStack(SIGUSR1, &sa);
}

#ifdef __OHOS__
void SignalManager::InstallSIGUSR2Handlers() const
{
    sigset_t mask;
    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction sa;
    sa.saSignalAction= HandleUnexpectedSIGUSR2;
    sa.scMask = mask;
    sa.scFlags = SA_SIGINFO | SA_ONSTACK;
    AddHandlerToSignalStack(SIGUSR2, &sa);
}

struct ProfDumpNode {
    int (*func)(void);
    ProfDumpNode *next;
};

std::atomic<ProfDumpNode*> profileDumpList {nullptr};

extern "C" void RegisterProfileDumpFunction(int (*func)(void))
{
    if (func == nullptr) {
        return;
    }

    // Check if func is already registered
    ProfDumpNode *current = profileDumpList.load(std::memory_order_relaxed);
    while (current != nullptr) {
        if (current->func == func) {
            return;
        }
        current = current->next;
    }

    // Not found, allocate and add
    ProfDumpNode *node = reinterpret_cast<ProfDumpNode*>(malloc(sizeof(ProfDumpNode)));
    if (node == nullptr) {
        LOG(RTLOG_FATAL, "Failed to allocate for ProfDumpNode");
        return;
    }

    node->func = func;
    ProfDumpNode *old = profileDumpList.load(std::memory_order_relaxed);
    do {
        node->next = old;
    } while (!profileDumpList.compare_exchange_weak(old, node, std::memory_order_relaxed));

    return;
}


extern "C" MRT_EXPORT
    void CJ_MRT_RegisterProfDumpFunc(int (*func)(void)) __attribute__((alias("RegisterProfileDumpFunction")));

bool SignalManager::HandleUnexpectedSIGUSR2(int sig, siginfo_t* info, void* context)
{
    ProfDumpNode *current = profileDumpList.load(std::memory_order_relaxed);
    if (current == nullptr) {
        LOG(RTLOG_INFO, "[CJ]: No Profile Dump Registered.");
        return true;
    }
    LOG(RTLOG_INFO, "[CJ]: Inst Profile Dump Start.");

    while (current != nullptr) {
        if (current->func != nullptr) {
            current->func();
        }
        current = current->next;
    }

    LOG(RTLOG_INFO, "[CJ]: Inst Profile Dump Finished.");
    return true;
}
#endif

bool SignalManager::HandleUnexpectedSIGUSR1(int sig, siginfo_t* info, void* context)
{
    Heap::GetHeap().GetCollectorResources().RequestHeapDump(GCTask::TaskType::TASK_TYPE_DUMP_HEAP);
    return true;
}

// Handle unexpected SIGSEGV
bool SignalManager::HandleUnexpectedSigsegv(int sig, siginfo_t* info, void* context)
{
    CheckSuspendState();
    // Do more functional things here.
    if (info != nullptr) {
        CheckStackOverflow(*info);
    }
    // pc/fa/si_addr already emitted at HandlerImpl entry (before user handlers).
    return false;
}

void SignalManager::InstallSegvHandler()
{
    sigset_t mask;
    // Allow some signals to be triggered when handling SIGSEGV
    CHECK_SIGNAL_CALL(sigfillset, (&mask), "sigfillset failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGABRT), "sigdelset SIGABRT failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGBUS), "sigdelset SIGBUS failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGFPE), "sigdelset SIGFPE failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGILL), "sigdelset SIGILL failed in InstallSegvHandler");
    CHECK_SIGNAL_CALL(sigdelset, (&mask, SIGSEGV), "sigdelset SIGSEGV failed in InstallSegvHandler");

    if (Runtime::Current().GetConcurrencyModel().GetStackGuardCheckFlag()) {
        // Alloc extra one page memory to handle stack overflow
        constexpr uint8_t minPageCount = 16;
        extraStackSize = std::max(AlignUp<uint32_t>(MINSIGSTKSZ, MapleRuntime::MRT_PAGE_SIZE),
                                  static_cast<uint32_t>(minPageCount * MapleRuntime::MRT_PAGE_SIZE));
        extraStack = PagePool::Instance().GetPage(extraStackSize);
        stack_t ss{};
        ss.ss_sp = extraStack;
        ss.ss_size = extraStackSize;
        CHECK_SIGNAL_CALL(sigaltstack, (&ss, nullptr), "sigaltstack failed in InstallSegvHandler");
    }

    CHECK_SIGNAL_CALL(sigemptyset, (&mask), "sigemptyset failed");
    SignalAction unexcept;
    unexcept.saSignalAction= HandleUnexpectedSigsegv;
    unexcept.scMask = mask;
    unexcept.scFlags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;
#ifdef __APPLE__
    AddHandlerToSignalStack(SIGBUS, &unexcept);
#else
    AddHandlerToSignalStack(SIGSEGV, &unexcept);
#endif
}

void SignalManager::AddHandlerToSignalStack(int signal, SignalAction* sa)
{
    SignalStack::InitializeSignalStack();

    if (signal <= 0 || signal >= _NSIG) {
        LOG(RTLOG_FATAL, "Invalid signal %d", signal);
    }

    SignalStack::GetStacks()[signal].AddHandler(sa);
    SignalStack::GetStacks()[signal].MarkSig(signal);
}

void SignalManager::RemoveHandlerFromSignalStack(int signal, bool (*fn)(int, siginfo_t*, void*))
{
    SignalStack::InitializeSignalStack();

    if (signal <= 0 || signal >= _NSIG) {
        LOG(RTLOG_FATAL, "Invalid signal %d", signal);
    }

    SignalStack::GetStacks()[signal].RemoveHandler(fn);
}

} // namespace MapleRuntime
