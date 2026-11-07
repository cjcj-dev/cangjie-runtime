#include "Cangjie.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

static bool Check(bool ok, const char* name)
{
    std::printf("ASSERT %s %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}

template<class T> static T Api(void* runtime, const char* name)
{
    auto result = reinterpret_cast<T>(dlsym(runtime, name));
    if (!Check(result != nullptr, name)) { std::exit(2); }
    return result;
}

int main(int argc, char** argv)
{
    if (argc != 6) { return 2; }
    const char* runtimePath = argv[1];
    const char* plugin = argv[2];
    const char* symbol = argv[3];
    const bool separate = std::strcmp(argv[4], "namespace") == 0;
    const char* lifecycle = argv[5];
    // No runtime query precedes the real first native/public producer. A base
    // namespace copy is a decoy: it must not satisfy secondary-image checks.
    void* decoy = nullptr;
    if (separate) {
        if (!Check(dlopen(runtimePath, RTLD_NOW | RTLD_LOCAL) != nullptr, "base-runtime-decoy")) { return 2; }
        decoy = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        if (!Check(decoy != nullptr, "base-plugin-decoy")) { return 2; }
    }
    void* runtime = dlmopen(separate ? LM_ID_NEWLM : LM_ID_BASE, runtimePath, RTLD_NOW | RTLD_LOCAL);
    if (!Check(runtime != nullptr, "runtime-image")) { return 2; }
    Lmid_t ns = LM_ID_BASE;
    if (!Check(dlinfo(runtime, RTLD_DI_LMID, &ns) == 0 && (separate ? ns != LM_ID_BASE : ns == LM_ID_BASE),
               "runtime-namespace")) { return 2; }
    auto init = Api<decltype(&InitCJRuntime)>(runtime, "InitCJRuntime");
    auto fini = Api<decltype(&FiniCJRuntime)>(runtime, "FiniCJRuntime");
    auto initLib = Api<decltype(&InitCJLibrary)>(runtime, "InitCJLibrary");
    auto load = Api<decltype(&LoadCJLibrary)>(runtime, "LoadCJLibrary");
    auto run = Api<decltype(&RunCJTask)>(runtime, "RunCJTask");
    auto result = Api<decltype(&GetTaskRet)>(runtime, "GetTaskRet");
    auto release = Api<decltype(&ReleaseHandle)>(runtime, "ReleaseHandle");
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    bool inited = std::strcmp(lifecycle, "live") == 0 || std::strcmp(lifecycle, "postfini") == 0 ||
                  std::strcmp(lifecycle, "live-exit") == 0 || std::strcmp(lifecycle, "public-live-exit") == 0;
    if (inited && !Check(init(&param) == E_OK, "runtime-init-before-load")) { return 2; }
    const bool publicLoad = std::strncmp(lifecycle, "public-", 7) == 0;
    if (publicLoad) {
        if (!Check(load(plugin) == E_OK, "public-first-load")) { return 2; }
        std::puts("TARGET public-normal-exit"); std::fflush(stdout);
        return 0;
    }
    void* native = dlmopen(ns, plugin, RTLD_NOW | RTLD_LOCAL);
    if (!Check(native != nullptr, "native-first-load")) { return 2; }
    link_map* original = nullptr;
    Lmid_t imageNs = LM_ID_BASE;
    void* function = dlsym(native, symbol);
    void* metadata = dlsym(native, "readscopeprobe:ReadScopeValue.ti");
    if (!Check(dlinfo(native, RTLD_DI_LINKMAP, &original) == 0 &&
               dlinfo(native, RTLD_DI_LMID, &imageNs) == 0 && imageNs == ns && function && metadata,
               "original-object-and-metadata")) { return 2; }
    if (decoy && !Check(dlsym(decoy, symbol) != function, "secondary-object-distinct-from-base")) { return 1; }
    const auto metadataWord = *static_cast<const uintptr_t*>(metadata);
    if (std::strcmp(lifecycle, "live-exit") == 0) {
        std::puts("TARGET initialized-normal-exit"); std::fflush(stdout);
        return 0;
    }
    if (std::strcmp(lifecycle, "postfini") == 0 && !Check(fini() == E_OK, "runtime-finalize-before-close")) { return 2; }
    std::puts("TARGET native-close"); std::fflush(stdout);
    if (!Check(dlclose(native) == 0, "native-close")) { return 1; }
    void* retained = dlmopen(ns, plugin, RTLD_NOW | RTLD_NOLOAD);
    if (!Check(retained != nullptr, "native-image-retained")) { return 1; }
    link_map* after = nullptr;
    Lmid_t afterNs = LM_ID_BASE;
    bool ok = Check(dlinfo(retained, RTLD_DI_LINKMAP, &after) == 0 && after == original &&
                    dlinfo(retained, RTLD_DI_LMID, &afterNs) == 0 && afterNs == imageNs,
                    "same-native-object-and-namespace");
    ok &= Check(dlsym(retained, symbol) == function &&
                dlsym(retained, "readscopeprobe:ReadScopeValue.ti") == metadata, "same-symbol-and-metadata");
    if (!Check(dlclose(retained) == 0, "release-observer-reference")) { return 1; }
    // No observation handle protects the image during these uses.
    ok &= Check(*static_cast<const uintptr_t*>(metadata) == metadataWord, "metadata-readable-after-close");
    if (std::strcmp(lifecycle, "never") == 0 || std::strcmp(lifecycle, "postfini") == 0) { return ok ? 0 : 1; }
    if (!inited && !Check(init(&param) == E_OK, "runtime-init-after-close")) { return 2; }
    if (!Check(initLib(plugin) == E_OK, "metadata-library-init-after-close")) { return 2; }
    auto task = run(reinterpret_cast<CJTaskFunc>(function), nullptr);
    if (!Check(task != nullptr, "managed-task-after-close")) { return 2; }
    void* value = nullptr;
    ok &= Check(result(task, &value) == 0 && reinterpret_cast<uintptr_t>(value) == 37,
                "managed-allocation-value-after-close");
    release(task);
    ok &= Check(fini() == E_OK, "runtime-finalize");
    return ok ? 0 : 1;
}
