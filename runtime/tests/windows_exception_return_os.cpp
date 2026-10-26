// Consume the installed DLL's actual Windows return-context implementation.
// Seeded native-frame cases exercise RtlVirtualUnwind, not managed stackmaps.
// The restore case resumes a real RtlCaptureContext continuation; no fake RA.
#include <windows.h>
#include <sys/types.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Exception/CalleeSavedRegisterContext.h"

using MapleRuntime::CalleeSavedRegisterContext;
using UnwindReturn = void (*)(CONTEXT*, void*, uint32_t);
using Transfer = void (*)(CONTEXT*, const CalleeSavedRegisterContext*, uintptr_t);
static volatile unsigned resumed = 0;
static volatile unsigned destructed = 0;

struct NativeCleanup {
    ~NativeCleanup() { ++destructed; }
};

__attribute__((noinline)) static void NativeCallback()
{
    NativeCleanup cleanup;
}

__attribute__((noinline)) static bool CheckRestore(Transfer transfer)
{
    CONTEXT captured{};
    RtlCaptureContext(&captured);
    if (resumed != 0) return resumed == 1 && destructed == 2;
    CalleeSavedRegisterContext managed{};
    managed.rbx = captured.Rbx; managed.rsi = captured.Rsi; managed.rdi = captured.Rdi;
    managed.r12 = captured.R12; managed.r13 = captured.R13;
    managed.r14 = captured.R14; managed.r15 = captured.R15;
    managed.rbp = captured.Rbp; managed.rsp = captured.Rsp;
    std::memcpy(&managed.xmm6, &captured.Xmm6, 10 * sizeof(M128A));
    {
        NativeCleanup outer;
        NativeCallback();
    }
    if (destructed != 2) return false;
    resumed = 1;
    transfer(&captured, &managed, captured.Rip);
    return false;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) { std::fprintf(stderr, "load error=%lu\n", GetLastError()); return 3; }
    auto unwind = reinterpret_cast<UnwindReturn>(GetProcAddress(module, "MRT_UnwindWindowsRuntimeReturn"));
    auto transfer = reinterpret_cast<Transfer>(GetProcAddress(module, "MRT_TransferWindowsException"));
    const auto complete = reinterpret_cast<DWORD64>(GetProcAddress(module, "MRT_CompleteWindowsExceptionReturn"));
    if (unwind == nullptr || transfer == nullptr || complete == 0) return 4;
    struct Frame { const char* name; unsigned size; unsigned cleanup; bool c2n; };
    const Frame frames[] = {
        {"CJ_MCC_C2NStub", 368, 16, true},
        {"CJ_MCC_ThrowException", 96, 0, false},
        {"CJ_MCC_OnFinalizerCreated", 96, 0, false},
        {"CJ_MCC_ReferenceInvokeFinalize", 96, 0, false},
        {"CJ_MCC_NewObject", 96, 0, false},
        {"CJ_MCC_ThrowStackOverflowError", 96, 0, false},
        {"CJ_MCC_ExclusiveScope", 208, 0, false}
    };
    for (const auto& layout : frames) {
        const auto entry = reinterpret_cast<DWORD64>(GetProcAddress(module, layout.name));
        DWORD64 image = 0;
        auto* function = RtlLookupFunctionEntry(entry, &image, nullptr);
        if (entry == 0 || function == nullptr) return 5;
        auto* info = reinterpret_cast<const unsigned char*>(image + function->UnwindData);
        if ((info[0] & 7) != 1 || info[1] == 0) return 6;
        // Use the actual boundary's return from its bridge CALL, not an
        // invented PC in the middle of an instruction or an epilogue.
        std::vector<DWORD64> returnSites;
        for (DWORD64 pc = entry + info[1]; pc + 5 <= image + function->EndAddress; ++pc) {
            if (*reinterpret_cast<const unsigned char*>(pc) != 0xE8) continue;
            int32_t displacement = 0;
            std::memcpy(&displacement, reinterpret_cast<const void*>(pc + 1), 4);
            if (pc + 5 + displacement == complete) returnSites.push_back(pc + 5);
        }
        if (returnSites.size() != (layout.c2n ? 2U : 1U)) return 7;
        alignas(16) std::array<unsigned char, 16384> storage{};
        const auto frame = reinterpret_cast<DWORD64>(storage.data() + 8192);
        const DWORD64 callerPc = reinterpret_cast<DWORD64>(&main);
        for (const auto returnSite : returnSites) for (unsigned dynamic : {0U, 80U, 4096U}) {
            storage.fill(0);
            CONTEXT original{};
            original.ContextFlags = CONTEXT_FULL;
            original.Rbx = 0x1111; original.Rbp = 0x2222; original.Rsi = 0x3333;
            original.Rdi = 0x4444; original.R12 = 0x5555; original.R13 = 0x6666;
            original.R14 = 0x7777; original.R15 = 0x8888;
            auto put = [](DWORD64 address, const void* value, size_t size) {
                std::memcpy(reinterpret_cast<void*>(address), value, size);
            };
            const DWORD64 saved[] = {original.R15, original.R14, original.R13, original.R12,
                                     original.Rdi, original.Rsi, original.Rbx};
            for (unsigned i = 0; i < 7; ++i) put(frame - 8 - 8 * i, &saved[i], 8);
            M128A vectors[10]{};
            for (unsigned i = 0; i < 10; ++i) {
                vectors[i].Low = 0x1234567800000000ULL + i;
                vectors[i].High = 0x2345678900000000LL + i;
                if (layout.c2n) put(frame - 176 - 16 * i, &vectors[i], 16);
            }
            std::memcpy(&original.Xmm6, vectors, sizeof(vectors));
            put(frame, &original.Rbp, 8); put(frame + 8, &callerPc, 8);
            CONTEXT context = original;
            context.Rbp = frame;
            if (layout.c2n) context.Rsi = frame - layout.size;
            context.Rsp = frame - layout.size - dynamic;
            context.Rip = returnSite;
            std::memset(reinterpret_cast<void*>(context.Rsp), 0xA5, 32);
            unwind(&context, reinterpret_cast<void*>(frame), layout.cleanup);
            const bool ok = context.Rip == callerPc && context.Rsp == frame + 16 + layout.cleanup &&
                context.Rbp == original.Rbp && context.Rbx == original.Rbx && context.Rsi == original.Rsi &&
                context.Rdi == original.Rdi && context.R12 == original.R12 && context.R13 == original.R13 &&
                context.R14 == original.R14 && context.R15 == original.R15 &&
                std::memcmp(&context.Xmm6, &original.Xmm6, sizeof(vectors)) == 0;
            std::printf("EH_OS_NATIVE_RETURN name=%s dynamic=%u cleanup=%u pass=%d\n",
                        layout.name, dynamic, layout.cleanup, ok);
            if (!ok) return 1;
        }
    }
    const bool restored = CheckRestore(transfer);
    std::printf("EH_OS_CONTEXT_RESTORE pass=%d native_cleanup=%u\n", restored, destructed);
    FreeLibrary(module);
    return restored ? 0 : 1;
}
