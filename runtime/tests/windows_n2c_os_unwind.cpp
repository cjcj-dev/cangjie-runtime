// Native Windows consumer of the installed product's public transition stubs.
// This checks RtlVirtualUnwind, not a replacement stub or a language invocation.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 2) return 2; // absolute path to the product DLL
    HMODULE module = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) { std::fprintf(stderr, "load error=%lu\n", GetLastError()); return 3; }
    struct Frame { const char* name; unsigned size; unsigned gpr; unsigned xmm; };
    for (const auto& layout : {Frame{"CJ_MCC_N2CStub", 416, 72, 240},
                               Frame{"CJ_MCC_C2NStub", 368, 8, 176}}) {
    auto entry = reinterpret_cast<DWORD64>(GetProcAddress(module, layout.name));
    DWORD64 image = 0;
    auto* function = RtlLookupFunctionEntry(entry, &image, nullptr);
    if (entry == 0 || function == nullptr) return 4;
    auto* info = reinterpret_cast<const unsigned char*>(image + function->UnwindData);
    if ((info[0] & 7) != 1 || (info[3] & 15) != 6 || (info[3] >> 4) != 0) return 5;

    alignas(16) std::array<unsigned char, 16384> storage{};
    const auto frame = reinterpret_cast<DWORD64>(storage.data() + 8192);
    const auto entrySp = frame + 8;
    const auto callerPc = reinterpret_cast<DWORD64>(&main);
    auto put = [](DWORD64 address, const void* value, size_t bytes) {
        std::memcpy(reinterpret_cast<void*>(address), value, bytes);
    };
    for (unsigned dynamic : {0U, 32U, 80U, 4096U}) {
        storage.fill(0);
        CONTEXT original{};
        original.Rbx = 0x11111111; original.Rbp = 0x22222222;
        original.Rsi = 0x33333333; original.Rdi = 0x44444444;
        original.R12 = 0x55555555; original.R13 = 0x66666666;
        original.R14 = 0x77777777; original.R15 = 0x88888888;
        M128A vectors[10]{};
        for (int i = 0; i < 10; ++i) {
            vectors[i].Low = 0x1234567800000000ULL + i;
            vectors[i].High = 0x2345678900000000LL + i;
            put(frame - layout.xmm - 16 * i, &vectors[i], 16);
        }
        std::memcpy(&original.Xmm6, vectors, sizeof(vectors));
        const DWORD64 saved[] = {original.R15, original.R14, original.R13, original.R12,
                                 original.Rdi, original.Rsi, original.Rbx};
        for (int i = 0; i < 7; ++i) put(frame - layout.gpr - 8 * i, &saved[i], 8);
        if (layout.gpr == 72) {
            put(frame - 32, &original.Rsi, 8); // N2C frame-register push
            for (int i = 1; i <= 3; ++i) {
                const DWORD64 contextWord = 0xBAD000 + i;
                put(frame - 8 * i, &contextWord, 8);
            }
        }
        put(frame, &original.Rbp, 8); put(entrySp, &callerPc, 8);
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        context.Rbp = frame;
        context.Rsi = frame - layout.size;
        context.Rsp = context.Rsi - dynamic;
        context.Rip = entry + info[1] + 1;
        std::memset(reinterpret_cast<void*>(context.Rsp), 0xA5, 32);
        DWORD64 establisher = 0;
        PVOID handlerData = nullptr;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context,
                         &handlerData, &establisher, nullptr);
        bool ok = context.Rip == callerPc && context.Rsp == entrySp + 8 &&
            context.Rbx == original.Rbx && context.Rbp == original.Rbp &&
            context.Rsi == original.Rsi && context.Rdi == original.Rdi &&
            context.R12 == original.R12 && context.R13 == original.R13 &&
            context.R14 == original.R14 && context.R15 == original.R15 &&
            std::memcmp(&context.Xmm6, &original.Xmm6, 10 * sizeof(M128A)) == 0;
        std::printf("TRANSITION_OS_UNWIND stub=%s dynamic=%u pass=%d\n", layout.name, dynamic, ok);
        if (!ok) return 1;
    }
    }
    FreeLibrary(module);
    return 0;
}
