#include "Cangjie.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

static bool Check(bool ok, const char* name)
{
    std::printf("ASSERT %s %s\n", name, ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok;
}
int main(int argc, char** argv)
{
    if (argc != 7) { return 2; }
    const bool separate = std::strcmp(argv[6], "namespace") == 0;
    void* runtime = dlmopen(separate ? LM_ID_NEWLM : LM_ID_BASE, argv[1], RTLD_NOW | RTLD_LOCAL);
    Lmid_t ns = LM_ID_BASE;
    if (!Check(runtime && dlinfo(runtime, RTLD_DI_LMID, &ns) == 0 &&
               (separate ? ns != LM_ID_BASE : ns == LM_ID_BASE), "loader-runtime-namespace")) { return 1; }
    auto load = reinterpret_cast<decltype(&LoadCJLibrary)>(dlsym(runtime, "LoadCJLibrary"));
    auto find = reinterpret_cast<decltype(&FindCJSymbol)>(dlsym(runtime, "FindCJSymbol"));
    if (!Check(load && find, "loader-public-api")) { return 1; }
    // An unrelated LOCAL observer is loaded before the public provider. Its
    // RTLD_DEFAULT query tests global visibility, not the provider's own scope.
    void* probe = dlmopen(ns, argv[2], RTLD_NOW | RTLD_LOCAL);
    auto visible = reinterpret_cast<int(*)()>(probe ? dlsym(probe, "readscope_global_visible") : nullptr);
    auto nullNs = reinterpret_cast<long(*)()>(probe ? dlsym(probe, "readscope_null_namespace") : nullptr);
    if (!Check(visible && nullNs && visible() == 0, "provider-initially-absent")) { return 1; }
    std::printf("TARGET namespace-probe actual=%ld null-handle=%ld\n", static_cast<long>(ns), nullNs());
    if (!Check(load(argv[3]) == E_OK, "public-native-provider")) { return 1; }
    auto value = reinterpret_cast<int(*)()>(find(argv[3], "readscope_namespace_value"));
    if (!Check(value && value() == 37, "public-explicit-symbol")) { return 1; }
    void* provider = dlmopen(ns, argv[3], RTLD_NOW | RTLD_NOLOAD);
    Lmid_t providerNs = -1;
    if (!Check(provider && dlinfo(provider, RTLD_DI_LMID, &providerNs) == 0 && providerNs == ns &&
               dlsym(provider, "readscope_namespace_value") == reinterpret_cast<void*>(value),
               "provider-same-object-and-namespace")) { return 1; }
    if (!Check(visible() == (separate ? 0 : 1), "namespace-symbol-visibility")) { return 1; }
    const int unbound = load(argv[4]);
    if (!Check(separate ? unbound != E_OK : unbound == E_OK, "implicit-global-dependency-contract")) { return 1; }
    if (!Check(load(argv[5]) == E_OK, "explicit-needed-dependency-load")) { return 1; }
    auto dependent = reinterpret_cast<int(*)()>(find(argv[5], "readscope_namespace_dependent"));
    if (!Check(dependent && dependent() == 37, "explicit-needed-dependency-symbol")) { return 1; }
    if (!Check(dlclose(provider) == 0 && dlclose(probe) == 0, "native-observers-released")) { return 1; }
    return 0;
}
