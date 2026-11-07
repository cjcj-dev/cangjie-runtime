#include "Cangjie.h"
#include "Loader/ElfUnloadQuiescence.h"
#include <cstdint>

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
    if (argc != 5) { return 2; }
    const char* mode = argv[1];
    const char* plugin = argv[2];
    const char* symbol = argv[3];
    // Deliberately no runtime init or reader before this producer.
    if (std::strcmp(mode, "live-exit-order-ba") == 0 &&
        !Check(LoadCJLibrary(argv[4]) == E_OK, "second-image-loaded-first")) { return 2; }
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
    if (std::strncmp(mode, "live-exit", 9) == 0) {
        if (std::strcmp(mode, "live-exit-order-ab") == 0 &&
            !Check(LoadCJLibrary(argv[4]) == E_OK, "second-image-loaded-last")) { return 2; }
        RuntimeParam param {};
        param.coParam.processorNum = 2;
        if (!Check(InitCJRuntime(&param) == E_OK, "init-after-public-load")) { return 2; }
        if (std::strcmp(mode, "live-exit-metadata") == 0) {
            if (!Check(InitCJLibrary(plugin) == E_OK, "library-metadata-init")) { return 2; }
            void* function = FindCJSymbol(plugin, symbol);
            void* metadata = FindCJSymbol(plugin, "readscopeprobe:ReadScopeValue.ti");
            using MapleRuntime::ElfUnloadQuiescence;
            ElfUnloadQuiescence::ReadScope reader;
            ElfUnloadQuiescence::AssertReaderActive();
            if (!Check(function && metadata &&
                       ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<uintptr_t>(function)) &&
                       ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<uintptr_t>(metadata)),
                       "registered-code-and-metadata-under-reader")) { return 1; }
            Dl_info codeInfo {}, metadataInfo {};
            if (!Check(dladdr(function, &codeInfo) && dladdr(metadata, &metadataInfo) &&
                       codeInfo.dli_fbase == metadataInfo.dli_fbase, "same-image-metadata-identity")) { return 1; }
            volatile uintptr_t word = *static_cast<const uintptr_t*>(metadata);
            (void)word;
            if (!Check(true, "metadata-read-under-reader")) { return 1; }
        }
        std::puts("TARGET initialized-normal-exit-after-public-first-load");
        std::fflush(stdout);
        return 0;
    }
    auto finish = [](bool ok) {
        RuntimeParam param {};
        param.coParam.processorNum = 2;
        ok &= Check(InitCJRuntime(&param) == E_OK, "path-control-init");
        ok &= Check(FiniCJRuntime() == E_OK, "path-control-finalize");
        return ok ? 0 : 1;
    };
    if (std::strcmp(mode, "missing") == 0) {
        bool ok = Check(LoadCJLibrary(argv[4]) != E_OK, "missing-same-basename-rejected");
        // Complete runtime shutdown so R1 cannot mask this path assertion.
        return finish(ok);
    }
    if (std::strcmp(mode, "distinct") == 0) {
        bool ok = Check(LoadCJLibrary(argv[4]) == E_OK, "distinct-same-basename-load");
        void* second = dlopen(argv[4], RTLD_NOW | RTLD_NOLOAD);
        ok &= Check(second != nullptr, "distinct-native-image-present");
        // RTLD_NOLOAD observes what the public request actually loaded; it
        // cannot make an absent second image appear as a side effect.
        if (second != nullptr) { dlclose(second); }
        return finish(ok);
    }
    if (std::strcmp(mode, "public-freeze") != 0 && std::strcmp(mode, "native-freeze") != 0) { return 2; }
    setenv("CJ_READSCOPE_FAST", "0", 1);
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    if (!Check(InitCJRuntime(&param) == E_OK, "runtime-init-after-toggle")) { return 2; }
    // Native-first production must retain before the public API takes a handle.
    // Drop only the extra reference, leaving the public owner intact.
    if (native != nullptr) {
        if (!Check(LoadCJLibrary(plugin) == E_OK, "public-adopt-native-image")) { return 2; }
        dlclose(native);
    }
    void* before = FindCJSymbol(plugin, symbol);
    bool ok = Check(before != nullptr, "symbol-before-close");
    ok &= Check(UnloadCJLibrary(plugin) == E_OK, "public-close-after-toggle");
    void* after = FindCJSymbol(plugin, symbol);
    ok &= Check(after == before, "first-producer-retained-with-old-env-zero");
    ok &= Check(FiniCJRuntime() == E_OK, "runtime-finalize");
    return ok ? 0 : 1;
}
