// Product-path probe for the stable MTable read APIs.

#include "Cangjie.h"
#include "LoaderManager.h"
#include "ObjectModel/MClass.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace MapleRuntime;

extern "C" void* CJ_MCC_LoadPackage(const char* path);
extern "C" U32 CJ_MCC_GetPackageNumOfTypeInfos(void* packageInfo);
extern "C" TypeInfo* CJ_MCC_GetPackageTypeInfo(void* packageInfo, U32 index);
extern "C" FuncPtr* CJ_MCC_GetMTable(TypeInfo* ti, TypeInfo* itf);
extern "C" TypeInfo* CJ_MCC_GetMethodOuterTI(TypeInfo* ti, TypeInfo* itf, U64 index);

extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadNativeBlock() {}
extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadMaybeNativeBlock() {}

namespace {
struct StableInterleaveState {
    std::atomic<bool> active { false };
    std::atomic<bool> entered { false };
    std::atomic<bool> invalidationWaitEntered { false };
    std::atomic<bool> invalidationDescMatched { false };
    std::atomic<bool> release { false };
    const MTableDesc* expectedDesc { nullptr };
};

StableInterleaveState* gStableInterleave[2] { nullptr, nullptr };
std::atomic<bool> gStableMutationAttempt { false };

extern "C" __attribute__((visibility("default"))) void MRT_TestStableHitPause(U32 api)
{
    if (api >= 2) {
        return;
    }
    auto* state = gStableInterleave[api];
    if (state == nullptr || !state->active.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!state->entered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    while (!state->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

extern "C" __attribute__((visibility("default"))) void MRT_TestStableMutationAttempt()
{
    gStableMutationAttempt.store(true, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) void MRT_TestStableInvalidationWait(const void* desc)
{
    for (auto* state : gStableInterleave) {
        if (state == nullptr || !state->active.load(std::memory_order_acquire)) {
            continue;
        }
        state->invalidationDescMatched.store(desc == state->expectedDesc, std::memory_order_release);
        state->invalidationWaitEntered.store(true, std::memory_order_release);
        return;
    }
}
} // namespace

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

bool FindTypesFromLoader(LoaderManager* loader, const char* packageName, TypeInfo*& klass, TypeInfo*& itf)
{
    const std::string prefix(packageName);
    klass = loader->FindTypeInfoFromLoadedFiles((prefix + ":UnloadProbeTypeWithInterface").c_str());
    itf = loader->FindTypeInfoFromLoadedFiles((prefix + ":UnloadProbeInterface").c_str());
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

bool FindInterleaveTypes(LoaderManager* loader, TypeInfo*& klass, TypeInfo*& hitItf, TypeInfo*& recursiveItf)
{
    klass = loader->FindTypeInfoFromLoadedFiles("elfunloadprobe:UnloadProbeTypeWithInterface");
    hitItf = loader->FindTypeInfoFromLoadedFiles("elfunloadprobe:UnloadProbeHitInterface");
    recursiveItf = loader->FindTypeInfoFromLoadedFiles("elfunloadprobe:UnloadProbeInterface");
    return klass != nullptr && hitItf != nullptr && recursiveItf != nullptr;
}

bool RunStableInterleave(LoaderManager* loader, const char* updatePath0, const char* updatePath1)
{
#ifdef BROKEN_NO_STABLE_WAIT
    (void)loader;
#endif
    TypeInfo* klass = nullptr;
    TypeInfo* hitItf = nullptr;
    TypeInfo* recursiveItf = nullptr;
    if (!FindInterleaveTypes(loader, klass, hitItf, recursiveItf)) {
        std::fprintf(stderr, "interleave types not found\n");
        return false;
    }
    // Prime both real API paths so the subsequent reader enters a published
    // stable hit.  The writer still goes through the public MTable API and
    // may recursively emplace the inherited interface entry.
    if (CJ_MCC_GetMTable(klass, hitItf) == nullptr ||
        CJ_MCC_GetMethodOuterTI(klass, hitItf, 0) == nullptr) {
        std::fprintf(stderr, "interleave hit APIs did not prime\n");
        return false;
    }

    bool allPassed = true;
    const char* updatePaths[2] = { updatePath0, updatePath1 };
    const char* updateInterfaces[2] = {
        "elfunloadprobe_update0:UnloadProbeMutationInterface0",
        "elfunloadprobe_update1:UnloadProbeMutationInterface1"
    };
    for (U32 api = 0; api < 2; ++api) {
        StableInterleaveState state;
        state.expectedDesc = klass->GetMTableDesc();
        if (state.expectedDesc == nullptr) {
            std::fprintf(stderr, "stable API %u has no target mtable descriptor\n", api);
            return false;
        }
        gStableInterleave[api] = &state;
        state.active.store(true, std::memory_order_release);
        std::atomic<bool> readerDone { false };
        std::atomic<bool> readerValue { false };
        std::atomic<bool> writerDone { false };
        std::atomic<bool> writerValue { false };
        gStableMutationAttempt.store(false, std::memory_order_release);
        std::thread reader([&, api] {
            bool ok = api == 0 ? CJ_MCC_GetMTable(klass, hitItf) != nullptr
                               : CJ_MCC_GetMethodOuterTI(klass, hitItf, 0) != nullptr;
            readerValue.store(ok, std::memory_order_release);
            readerDone.store(true, std::memory_order_release);
        });
        const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!state.entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < enteredDeadline) {
            std::this_thread::yield();
        }
        if (!state.entered.load(std::memory_order_acquire)) {
            state.release.store(true, std::memory_order_release);
            reader.join();
            gStableInterleave[api] = nullptr;
            std::fprintf(stderr, "stable API %u did not enter its guard hook\n", api);
            return false;
        }
        std::thread writer([&] {
            // Loading the second image is the real late extension/update
            // entry.  Its metadata targets the already-loaded class, so the
            // loader must publish a new mtable entry under the same table
            // mutex while this reader owns the stable snapshot.
            void* updatePackage = CJ_MCC_LoadPackage(updatePaths[api]);
            auto* updateItf = loader->FindTypeInfoFromLoadedFiles(updateInterfaces[api]);
            bool ok = reinterpret_cast<uintptr_t>(updatePackage) > 3 && updateItf != nullptr &&
                CJ_MCC_GetMTable(klass, updateItf) != nullptr;
            writerValue.store(ok, std::memory_order_release);
            writerDone.store(true, std::memory_order_release);
        });
        // Do not infer writer progress from a wall-clock sleep.  The runtime
        // hook fires after this descriptor's snapshot is detached and exactly
        // before the reader-quiescence wait.  Both the normal and broken
        // builds must reach this same rendezvous before we inspect writerDone.
        const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!state.invalidationWaitEntered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < waitDeadline) {
            std::this_thread::yield();
        }
        const bool waitReached = state.invalidationWaitEntered.load(std::memory_order_acquire);
        const bool descMatched = state.invalidationDescMatched.load(std::memory_order_acquire);
        const bool blocked = waitReached && descMatched && !writerDone.load(std::memory_order_acquire);
        state.release.store(true, std::memory_order_release);
        reader.join();
        writer.join();
        state.active.store(false, std::memory_order_release);
        gStableInterleave[api] = nullptr;

        // After the reader releases the guard, the mutation must complete and
        // both APIs must observe valid metadata from the same real image.
        auto* updateItf = loader->FindTypeInfoFromLoadedFiles(updateInterfaces[api]);
        const bool recovered = writerValue.load(std::memory_order_acquire) && updateItf != nullptr &&
            CJ_MCC_GetMTable(klass, updateItf) != nullptr &&
            CJ_MCC_GetMethodOuterTI(klass, hitItf, 0) != nullptr;
        const bool mutationAttempted = gStableMutationAttempt.load(std::memory_order_acquire);
        const bool passed = waitReached && descMatched && blocked && mutationAttempted &&
            readerDone.load() && readerValue.load() && recovered;
        std::printf("STABLE_API_INTERLEAVE api=%u wait=%d desc=%d blocked=%d mutation=%d released=%d writer=%d recovered=%d result=%s\n",
                    api, waitReached, descMatched, blocked, mutationAttempted,
                    !state.release.load() ? 0 : 1,
                    writerDone.load(), recovered, passed ? "PASS" : "FAIL");
        allPassed = allPassed && passed;
    }
    return allPassed;
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
    bool typesFound = FindTypes(package, klass, itf);
    if (!typesFound) {
        typesFound = FindTypesFromLoader(loader, "elfunloadprobe", klass, itf);
    }
    const std::string className = typesFound ? klass->GetName() : std::string();
    const bool published = typesFound &&
        loader->FindTypeInfoFromLoadedFiles(className.c_str()) == klass;
    const bool reads = published && ReadMTableApis(klass, itf);
    const std::string updatePath0 = std::string(plugin) + ".update0.so";
    const std::string updatePath1 = std::string(plugin) + ".update1.so";
    const bool stableInterleave = typesFound &&
        RunStableInterleave(loader, updatePath0.c_str(), updatePath1.c_str());
    // Drop the two extension images first.  Their metadata entries retain
    // the primary class image until they are unlinked, so primary unload is
    // checked only after the real update producers have drained.
    const int updateUnload0 = UnloadCJLibrary(updatePath0.c_str());
    const int updateUnload1 = UnloadCJLibrary(updatePath1.c_str());

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
        if (!reloaded) {
            reloaded = FindTypesFromLoader(loader, "elfunloadprobe_reload", klass, itf);
        }
        recovered = reloaded && ReadMTableApis(klass, itf);
    }
    const int reloadUnload = reloaded ? UnloadCJLibrary(reloadPath.c_str()) : E_ARGS;
    const bool passed = typesFound && published && reads && stableInterleave && unload == E_OK && removed &&
        reloaded && recovered && reloadUnload == E_OK && updateUnload0 == E_OK && updateUnload1 == E_OK;
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
