// Native installed-DLL entry for the three Windows gc_unit frame consumers.
// Link test_unwind_regressions.cpp and the candidate's real import library.
// This is a save-area/PE-table consumer, not a managed throw or GC lifecycle test.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "gc_unit/gc_unittest.hpp"

extern uintptr_t windowsC2NBodyEnd;

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    HMODULE product = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (product == nullptr) {
        std::fprintf(stderr, "load error=%lu\n", GetLastError());
        return 3;
    }
    // A different DLL found by the loader must not silently supply the tests.
    if (reinterpret_cast<uintptr_t>(GetProcAddress(product, "windowsC2NBodyEnd")) !=
        reinterpret_cast<uintptr_t>(&windowsC2NBodyEnd)) {
        std::fprintf(stderr, "frame consumer import does not bind the requested product DLL\n");
        FreeLibrary(product);
        return 4;
    }
    const char* names[] = {"WindowsExceptionBoundaryRealReturns", "WindowsExceptionSaveSlots",
                           "WindowsC2NPrivateCallerSP"};
    unsigned completed = 0;
    unsigned failed = 0;
    for (const auto* name : names) {
        unsigned selected = 0;
        for (const auto& test : MapleRuntime::GcUnit::Registry()) {
            if (std::strcmp(test.suite, "UnwindRegress") != 0 || std::strcmp(test.name, name) != 0) continue;
            ++selected;
            std::printf("[ RUN ] UnwindRegress.%s\n", name);
            std::fflush(stdout);
            try {
                test.fn();
                ++completed;
                std::printf("[ PASS ] UnwindRegress.%s\n", name);
            } catch (const std::exception& error) {
                ++failed;
                std::fprintf(stderr, "[ FAIL ] UnwindRegress.%s: %s\n", name, error.what());
            }
        }
        if (selected != 1) { ++failed; std::fprintf(stderr, "consumer selection count=%u: %s\n", selected, name); }
    }
    FreeLibrary(product);
    return completed == 3 && failed == 0 ? 0 : 1;
}
