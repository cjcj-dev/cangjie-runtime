#include "Cangjie.h"
#include "Loader/ElfUnloadQuiescence.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <dlfcn.h>

using MapleRuntime::ElfUnloadQuiescence;

static bool Check(bool ok, const char* name)
{
    std::printf("ASSERT %s %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 5) { return 2; }
    const char* mode = argv[1];
    const char* plugin = argv[2];
    const char* symbol = argv[3];
    const bool expected = std::strcmp(argv[4], "1") == 0;
    if (!Check(ElfUnloadQuiescence::NoUnloadMode() == expected, "activation")) { return 1; }
    // A later environment edit must not convert an outstanding slow reader
    // into a fast reader, or permit a writer after fast admission.
    {
        ElfUnloadQuiescence::ReadScope outer;
        setenv("CJ_READSCOPE_FAST", expected ? "0" : "1", 1);
        ElfUnloadQuiescence::ReadScope inner;
        ElfUnloadQuiescence::AssertReaderActive();
        if (!Check(ElfUnloadQuiescence::NoUnloadMode() == expected, "immutable-mode")) { return 1; }
    }
    std::vector<std::thread> readers;
    for (int n = 0; n < 4; ++n) {
        readers.emplace_back([]() {
            for (int i = 0; i < 1000; ++i) {
                ElfUnloadQuiescence::ReadScope outer;
                ElfUnloadQuiescence::ReadScope inner;
                ElfUnloadQuiescence::AssertReaderActive();
            }
        });
    }
    for (auto& thread : readers) { thread.join(); }
    if (std::strcmp(mode, "preinit-direct") == 0) {
        void* native = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!Check(native != nullptr, "preinit-dlopen")) { return 2; }
        std::puts("TARGET direct-close-before-init"); std::fflush(stdout);
        dlclose(native);
        return expected ? 3 : 0;
    }
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    if (!Check(InitCJRuntime(&param) == E_OK, "runtime-init")) { return 2; }
    if (std::strcmp(mode, "direct") == 0) {
        void* native = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!Check(native != nullptr, "direct-dlopen")) { return 2; }
        std::puts("TARGET direct-close-live"); std::fflush(stdout);
        dlclose(native);
        return expected ? 3 : (FiniCJRuntime() == E_OK ? 0 : 2);
    }
    if (!Check(LoadCJLibraryWithInit(plugin) == E_OK, "library-init")) { return 2; }
    void* before = FindCJSymbol(plugin, symbol);
    bool ok = Check(before != nullptr, "symbol-before");
    ok &= Check(UnloadCJLibrary(nullptr) != E_OK, "null-close-rejected");
    ok &= Check(UnloadCJLibrary("libreadscope-missing.so") != E_OK, "unknown-close-rejected");
    ok &= Check(UnloadCJLibrary(plugin) == E_OK, "library-close");
    void* after = FindCJSymbol(plugin, symbol);
    ok &= Check(expected ? after == before : after == nullptr, "close-lifetime");
    ok &= Check(LoadCJLibraryWithInit(plugin) == E_OK, "library-reopen");
    void* reopened = FindCJSymbol(plugin, symbol);
    ok &= Check(reopened != nullptr && (!expected || reopened == before), "reopen-symbol");
    ok &= Check(UnloadCJLibrary(plugin) == E_OK, "reopen-close");
    ok &= Check(FiniCJRuntime() == E_OK, "runtime-finalize");
    return ok ? 0 : 1;
}
