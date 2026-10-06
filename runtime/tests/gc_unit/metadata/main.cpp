// Isolated registry runner: no full-heap fixture or product implementation.
#include "gc_unittest.hpp"
#if defined(_WIN64)
#include <windows.h>
#include <crtdbg.h>
#elif defined(__linux__)
#include <link.h>
#endif
int main(int argc, char** argv)
{
#if defined(_WIN64)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#if defined(_WIN64)
    char modulePath[MAX_PATH] {};
    HMODULE module = GetModuleHandleA("libcangjie-runtime.dll");
    if (module == nullptr || !GetModuleFileNameA(module, modulePath, MAX_PATH)) { return 66; }
    std::fprintf(stderr, "RUNTIME_MODULE %s\n", modulePath);
#elif defined(__linux__)
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
        if (std::strstr(info->dlpi_name, "libcangjie-runtime.so") != nullptr) {
            std::fprintf(stderr, "RUNTIME_MODULE %s\n", info->dlpi_name);
        }
        return 0;
    }, nullptr);
#endif
    if (argc != 2) { return 64; }
    const std::string name(argv[1]);
    if (name == "--list") {
        for (const auto& test : MapleRuntime::GcUnit::Registry()) {
            std::printf("%s.%s\n", test.suite, test.name);
        }
        return MapleRuntime::GcUnit::Registry().empty() ? 64 : 0;
    }
    if (name == "--timeout-control") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }
    if (name == "--non-target-control") {
        std::fprintf(stderr, "NON_TARGET_CONTROL\n");
        std::fflush(stderr);
        std::raise(SIGSEGV);
        return 65;
    }
    if (name == "--abort-control") {
        std::fprintf(stderr, "ABORT_CONTROL\n");
        std::fflush(stderr);
        std::abort();
    }
    for (const auto& test : MapleRuntime::GcUnit::Registry()) {
        if (name != std::string(test.suite) + "." + test.name) { continue; }
        std::fprintf(stderr, "METADATA_CHILD %s\n", name.c_str());
        std::fflush(stderr);
        try {
            test.fn();
            std::fprintf(stderr, "METADATA_COMPLETE %s\n", name.c_str());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "METADATA_ASSERT %s %s\n", name.c_str(), error.what());
            return 1;
        }
    }
    std::fprintf(stderr, "UNKNOWN_FILTER %s\n", name.c_str());
    return 64;
}
