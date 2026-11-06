#include "Cangjie.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

static bool Check(bool ok, const char* name)
{
    std::printf("ASSERT %s %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 6) { return 2; }
    const char* mode = argv[1];
    const char* plugin = argv[2];
    const char* symbol = argv[3];
    const bool fast = std::strcmp(argv[4], "1") == 0;
    // Deliberately no runtime init, reader, or mode query before this producer.
    void* native = nullptr;
    if (std::strcmp(mode, "native-freeze") == 0) {
        native = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!Check(native != nullptr, "first-native-load")) { return 2; }
    } else if (!Check(LoadCJLibrary(plugin) == E_OK, "first-public-load")) {
        return 2;
    }
    if (std::strcmp(mode, "exit") == 0) {
        std::puts("TARGET load-only-normal-exit");
        std::fflush(stdout);
        return 0;
    }
    if (std::strcmp(mode, "missing") == 0) {
        bool ok = Check(LoadCJLibrary(argv[5]) != E_OK, "missing-same-basename-rejected");
        // Exit intentionally remains part of this public load-only consumer.
        return ok ? 0 : 1;
    }
    if (std::strcmp(mode, "distinct") == 0) {
        bool ok = Check(LoadCJLibrary(argv[5]) == E_OK, "distinct-same-basename-load");
        void* second = dlopen(argv[5], RTLD_NOW | RTLD_NOLOAD);
        ok &= Check(second != nullptr, "distinct-native-image-present");
        // RTLD_NOLOAD observes what the public request actually loaded; it
        // cannot make an absent second image appear as a side effect.
        if (second != nullptr) { dlclose(second); }
        return ok ? 0 : 1;
    }
    if (std::strcmp(mode, "public-freeze") != 0 && std::strcmp(mode, "native-freeze") != 0) { return 2; }
    setenv("CJ_READSCOPE_FAST", fast ? "0" : "1", 1);
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    if (!Check(InitCJRuntime(&param) == E_OK, "runtime-init-after-toggle")) { return 2; }
    // Native-first production must also freeze before the public API takes a
    // handle. Drop only the extra reference, leaving the public owner intact.
    if (native != nullptr) {
        if (!Check(LoadCJLibrary(plugin) == E_OK, "public-adopt-native-image")) { return 2; }
        dlclose(native);
    }
    void* before = FindCJSymbol(plugin, symbol);
    bool ok = Check(before != nullptr, "symbol-before-close");
    ok &= Check(UnloadCJLibrary(plugin) == E_OK, "public-close-after-toggle");
    void* after = FindCJSymbol(plugin, symbol);
    ok &= Check(fast ? after == before : after == nullptr, "first-producer-froze-mode");
    ok &= Check(FiniCJRuntime() == E_OK, "runtime-finalize");
    return ok ? 0 : 1;
}
