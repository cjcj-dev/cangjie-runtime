// Product-path probe for the stable MTable read APIs.

#include "Cangjie.h"
#include "LoaderManager.h"
#include "ObjectModel/MClass.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace MapleRuntime;

extern "C" void* CJ_MCC_LoadPackage(const char* path);
extern "C" U32 CJ_MCC_GetPackageNumOfTypeInfos(void* packageInfo);
extern "C" TypeInfo* CJ_MCC_GetPackageTypeInfo(void* packageInfo, U32 index);
extern "C" FuncPtr* CJ_MCC_GetMTable(TypeInfo* ti, TypeInfo* itf);
extern "C" TypeInfo* CJ_MCC_GetMethodOuterTI(TypeInfo* ti, TypeInfo* itf, U64 index);

extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadNativeBlock() {}
extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadMaybeNativeBlock() {}

namespace {
bool FindTypes(void* packageInfo, TypeInfo*& klass, TypeInfo*& itf)
{
    klass = nullptr;
    itf = nullptr;
    if (packageInfo == nullptr) {
        return false;
    }
    const U32 count = CJ_MCC_GetPackageNumOfTypeInfos(packageInfo);
    std::printf("PACKAGE_TYPES count=%u\n", count);
    for (U32 index = 0; index < count; ++index) {
        TypeInfo* ti = CJ_MCC_GetPackageTypeInfo(packageInfo, index);
        if (ti == nullptr) {
            continue;
        }
        std::printf("PACKAGE_TYPE index=%u name=%s interface=%d class=%d\n", index, ti->GetName(),
                    ti->IsInterface(), ti->IsClass());
        if (ti->IsInterface()) {
            itf = ti;
        } else if (ti->IsClass() &&
                   std::strstr(ti->GetName(), "UnloadProbeTypeWithInterface") != nullptr) {
            klass = ti;
        }
    }
    return klass != nullptr && itf != nullptr;
}

bool ReadMTableApis(TypeInfo* klass, TypeInfo* itf)
{
#ifdef BROKEN_NO_READ_APIS
    (void)klass;
    (void)itf;
    return false;
#else
    return CJ_MCC_GetMTable(klass, itf) != nullptr &&
        CJ_MCC_GetMethodOuterTI(klass, itf, 0) != nullptr;
#endif
}

int Run(const char* plugin)
{
    RuntimeParam param {};
    param.coParam.processorNum = 2;
    if (InitCJRuntime(&param) != E_OK) {
        std::fprintf(stderr, "runtime or loader initialization failed\n");
        return 2;
    }

    auto* loader = LoaderManager::GetInstance();
    void* package = CJ_MCC_LoadPackage(plugin);
    if (reinterpret_cast<uintptr_t>(package) <= 3) {
        std::fprintf(stderr, "CJ_MCC_LoadPackage failed: %p\n", package);
        (void)FiniCJRuntime();
        return 2;
    }
    TypeInfo* klass = nullptr;
    TypeInfo* itf = nullptr;
    const bool typesFound = FindTypes(package, klass, itf);
    const std::string className = typesFound ? klass->GetName() : std::string();
    const bool published = typesFound &&
        loader->FindTypeInfoFromLoadedFiles(className.c_str()) == klass;
    const bool reads = published && ReadMTableApis(klass, itf);

    const int unload = UnloadCJLibrary(plugin);
    const bool removed = typesFound &&
        loader->FindTypeInfoFromLoadedFiles(className.c_str()) == nullptr;

    const std::string reloadPath = std::string(plugin) + ".reload.so";
    bool reloaded = false;
    bool recovered = false;
    if (unload == E_OK) {
        package = CJ_MCC_LoadPackage(reloadPath.c_str());
        reloaded = reinterpret_cast<uintptr_t>(package) > 3;
        klass = nullptr;
        itf = nullptr;
        reloaded = reloaded && FindTypes(package, klass, itf);
        recovered = reloaded && ReadMTableApis(klass, itf);
    }
    const int reloadUnload = reloaded ? UnloadCJLibrary(reloadPath.c_str()) : E_ARGS;
    const bool passed = typesFound && published && reads && unload == E_OK && removed &&
        reloaded && recovered && reloadUnload == E_OK;
    std::printf("MTABLE_API_LIFECYCLE result=%s types=%d published=%d reads=%d unload=%d "
                "removed=%d reloaded=%d recovered=%d reload_unload=%d\n",
                passed ? "PASS" : "FAIL", typesFound, published, reads, unload, removed,
                reloaded, recovered, reloadUnload);
    const int rc = (FiniCJRuntime() == E_OK && passed) ? 0 : 1;
    return rc;
}
} // namespace

int main(int argc, char** argv)
{
    return argc == 2 ? Run(argv[1]) : 2;
}
