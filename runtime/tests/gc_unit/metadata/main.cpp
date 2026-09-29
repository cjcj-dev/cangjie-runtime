// Isolated registry runner: no full-heap fixture or product implementation.
#include "gc_unittest.hpp"
#if defined(_WIN64)
#include <windows.h>
#include <crtdbg.h>
#endif
int main(int argc, char** argv)
{
#if defined(_WIN64)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc != 2) { return 64; }
    const std::string name(argv[1]);
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
