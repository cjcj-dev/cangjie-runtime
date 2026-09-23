// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Cangjie.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include <dlfcn.h>

extern "C" void* MRT_TestElfUnloadReaderEnter();
extern "C" void MRT_TestElfUnloadReaderExit(void* token);
extern "C" bool MRT_TestElfUnloadAddressLinked(uintptr_t address);
extern "C" bool MRT_TestElfUnloadPending();
extern "C" size_t MRT_TestElfUnloadStaticRootCount();
extern "C" bool MRT_TestElfUnloadPackagePathPresent(const char* path);
extern "C" size_t MRT_TestElfUnloadPackageIndexSize();
extern "C" bool MRT_TestElfUnloadTypePresent(const char* name);
extern "C" size_t MRT_TestElfUnloadTypeIndexSize();
extern "C" bool MRT_TestElfUnloadFuncDescPresent(uintptr_t startPC);
extern "C" uintptr_t MRT_TestElfUnloadImageIdentity(uintptr_t address);
extern "C" bool MRT_TestElfUnloadImageIdentityLinked(uintptr_t imageIdentity);
extern "C" void MRT_TestElfUnloadResetDirectOrder();
extern "C" bool MRT_TestElfUnloadDirectOrderValid();
extern "C" bool MRT_TestElfUnloadDirectPreflightEntered();
extern "C" void* MRT_TestElfUnloadLibraryHandle(const char* libName);
extern "C" void MRT_TestElfUnloadEnableGcReaderPause();
extern "C" bool MRT_TestElfUnloadGcReaderPaused();
extern "C" void MRT_TestElfUnloadReleaseGcReaderPause();
extern "C" void MRT_TestElfUnloadEnablePackageReaderPause();
extern "C" bool MRT_TestElfUnloadPackageReaderPaused();
extern "C" void MRT_TestElfUnloadReleasePackageReaderPause();
extern "C" void MRT_TestElfUnloadSynchronize(uintptr_t imageAddress);
extern "C" void MRT_TestElfUnloadResetUnrelatedStw();
extern "C" bool MRT_TestElfUnloadUnrelatedStwHeld();
extern "C" void MRT_TestElfUnloadReleaseUnrelatedStw();
extern "C" void MRT_TestElfUnloadHoldUnrelatedStw();
extern "C" void CJ_MRT_ForceFullGC();
extern "C" void* CJ_MCC_LoadPackage(const char* path);
extern "C" const char* CJ_MCC_GetPackageVersion(void* packageInfo);
extern "C" const char* CJ_MCC_GetPackageName(void* packageInfo);
extern "C" uint32_t CJ_MCC_GetPackageNumOfTypeInfos(void* packageInfo);
extern "C" void MRT_PreInitializePackage(uint64_t address);
namespace {
std::atomic<bool> nativeEntered { false };
std::atomic<bool> nativeRelease { false };
std::atomic<bool> nativeBlockEnabled { false };
}

extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadNativeBlock()
{
    nativeEntered.store(true, std::memory_order_release);
    while (!nativeRelease.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

extern "C" __attribute__((visibility("default"))) void MRT_TestElfUnloadMaybeNativeBlock()
{
    if (nativeBlockEnabled.load(std::memory_order_acquire)) {
        MRT_TestElfUnloadNativeBlock();
    }
}

namespace {
bool WaitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void Result(const char* group, bool passed)
{
    std::printf("TEST name=%s result=%s\n", group, passed ? "PASS" : "FAIL");
}

bool InitRuntime(uint32_t processorNum = 2)
{
    RuntimeParam param {};
    param.coParam.processorNum = processorNum;
    return InitCJRuntime(&param) == E_OK;
}

int RunQueuedTaskReject(const char* plugin, const char* queuedSymbol,
                        const char* blockerPlugin, const char* blockerSymbol)
{
    if (!InitRuntime(1) || LoadCJLibraryWithInit(blockerPlugin) != E_OK ||
        LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or queue probe initialization failed\n");
        return 2;
    }
    auto blocker = reinterpret_cast<CJTaskFunc>(FindCJSymbol(blockerPlugin, blockerSymbol));
    auto queued = reinterpret_cast<CJTaskFunc>(FindCJSymbol(plugin, queuedSymbol));
    nativeEntered.store(false, std::memory_order_release);
    nativeRelease.store(false, std::memory_order_release);
    CJThreadHandle blockerTask = blocker == nullptr ? nullptr : RunCJTask(blocker, nullptr);
    bool blockerEntered = blockerTask != nullptr &&
        WaitFor([]() { return nativeEntered.load(std::memory_order_acquire); });
    CJThreadHandle queuedTask = blockerEntered && queued != nullptr ? RunCJTask(queued, nullptr) : nullptr;
    int firstUnload = queuedTask == nullptr ? E_ARGS : UnloadCJLibrary(plugin);
    bool rejected = queuedTask != nullptr && firstUnload != E_OK &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(queued));
    if (!rejected) {
        Result("ElfUnload.QueuedTaskReject", false);
        std::printf("DETAIL blocker_entered=%d queued=%d first_unload=%d\n",
                    blockerEntered, queuedTask != nullptr, firstUnload);
        std::fflush(nullptr);
        // Do not allow a deliberately disconnected queue guard to dispatch an
        // entry after its image has already been unmapped.
        std::_Exit(1);
    }

    nativeRelease.store(true, std::memory_order_release);
    void* blockerResult = nullptr;
    void* queuedResult = nullptr;
    bool blockerReturned = GetTaskRet(blockerTask, &blockerResult) == E_OK;
    bool queuedReturned = GetTaskRet(queuedTask, &queuedResult) == E_OK;
    ReleaseHandle(blockerTask);
    ReleaseHandle(queuedTask);
    int secondUnload = UnloadCJLibrary(plugin);
    int blockerUnload = UnloadCJLibrary(blockerPlugin);
    bool passed = blockerReturned && queuedReturned && secondUnload == E_OK && blockerUnload == E_OK;
    Result("ElfUnload.QueuedTaskReject", passed);
    std::printf("DETAIL blocker_entered=%d first_unload=%d blocker_returned=%d "
                "queued_returned=%d second_unload=%d blocker_unload=%d\n",
                blockerEntered, firstUnload, blockerReturned, queuedReturned, secondUnload, blockerUnload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunGcReaderDrain(const char* plugin, const char* nativeBlockSymbol)
{
    if (!InitRuntime(1) || LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or GC reader probe initialization failed\n");
        return 2;
    }
    auto nativeBlock = reinterpret_cast<CJTaskFunc>(FindCJSymbol(plugin, nativeBlockSymbol));
    nativeEntered.store(false, std::memory_order_release);
    nativeRelease.store(false, std::memory_order_release);
    CJThreadHandle task = nativeBlock == nullptr ? nullptr : RunCJTask(nativeBlock, nullptr);
    bool entered = task != nullptr && WaitFor([]() { return nativeEntered.load(std::memory_order_acquire); });
    MRT_TestElfUnloadEnableGcReaderPause();
    std::thread collector([]() { CJ_MRT_ForceFullGC(); });
    bool readerPaused = entered && WaitFor([]() { return MRT_TestElfUnloadGcReaderPaused(); });

    std::atomic<bool> synchronized { false };
    std::thread writer([&]() {
        MRT_TestElfUnloadSynchronize(reinterpret_cast<uintptr_t>(nativeBlock));
        synchronized.store(true, std::memory_order_release);
    });
    bool writerPending = readerPaused && WaitFor([]() { return MRT_TestElfUnloadPending(); });
    bool advancedWhileReaderHeld = writerPending && WaitFor([&]() {
        return synchronized.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));

    MRT_TestElfUnloadReleaseGcReaderPause();
    collector.join();
    writer.join();
    nativeRelease.store(true, std::memory_order_release);
    void* taskResult = nullptr;
    bool returned = GetTaskRet(task, &taskResult) == E_OK;
    ReleaseHandle(task);
    int unload = UnloadCJLibrary(plugin);
    bool passed = entered && readerPaused && writerPending && !advancedWhileReaderHeld &&
        synchronized.load(std::memory_order_acquire) && returned && unload == E_OK;
    Result("ElfUnload.GcEntryReader", passed);
    std::printf("DETAIL entered=%d reader_paused=%d writer_pending=%d advanced=%d "
                "synchronized=%d returned=%d unload=%d\n",
                entered, readerPaused, writerPending, advancedWhileReaderHeld,
                synchronized.load(), returned, unload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunNativeFrameReject(const char* plugin, const char* markerSymbol, const char* activeSymbol)
{
    if (!InitRuntime() || LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or plugin initialization failed\n");
        return 2;
    }
    void* marker = FindCJSymbol(plugin, markerSymbol);
    auto startActive = reinterpret_cast<CJTaskFunc>(FindCJSymbol(plugin, activeSymbol));
    nativeEntered.store(false, std::memory_order_release);
    nativeRelease.store(false, std::memory_order_release);
    CJThreadHandle starterTask = startActive == nullptr ? nullptr : RunCJTask(startActive, nullptr);
    void* starterResult = nullptr;
    bool starterReturned = starterTask != nullptr && GetTaskRet(starterTask, &starterResult) == E_OK;
    if (starterTask != nullptr) {
        ReleaseHandle(starterTask);
    }
    bool entered = starterReturned &&
        WaitFor([]() { return nativeEntered.load(std::memory_order_acquire); });
    int firstUnload = entered ? UnloadCJLibrary(plugin) : E_ARGS;
    bool rejected = entered && firstUnload != E_OK && marker != nullptr &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));
    if (!rejected) {
        Result("ElfUnload.NativeFrameReject", false);
        std::printf("DETAIL entered=%d first_unload=%d\n", entered, firstUnload);
        std::fflush(nullptr);
        // If the active-frame bearing point was cut, the image may already be
        // unmapped. Do not release the native call back into that image.
        std::_Exit(1);
    }

    nativeRelease.store(true, std::memory_order_release);
    int secondUnload = E_ARGS;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        secondUnload = UnloadCJLibrary(plugin);
        if (secondUnload == E_OK) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool passed = starterReturned && secondUnload == E_OK &&
        !MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));
    Result("ElfUnload.NativeFrameReject", passed);
    std::printf("DETAIL starter_returned=%d entered=%d first_unload=%d second_unload=%d\n",
                starterReturned, entered, firstUnload, secondUnload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunCoreUnload(const char* plugin, const char* markerSymbol, const char* typeName)
{
    if (!InitRuntime()) {
        std::fprintf(stderr, "runtime initialization failed\n");
        return 2;
    }
    size_t rootsBefore = MRT_TestElfUnloadStaticRootCount();
    size_t typesBefore = MRT_TestElfUnloadTypeIndexSize();
    size_t packagesBefore = MRT_TestElfUnloadPackageIndexSize();
    if (LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "plugin initialization failed\n");
        return 2;
    }
    void* marker = FindCJSymbol(plugin, markerSymbol);
    if (marker == nullptr) {
        std::fprintf(stderr, "FindCJSymbol failed for %s\n", markerSymbol);
        return 2;
    }
    size_t rootsLoaded = MRT_TestElfUnloadStaticRootCount();
    size_t typesLoaded = MRT_TestElfUnloadTypeIndexSize();
    size_t packagesLoaded = MRT_TestElfUnloadPackageIndexSize();
    uintptr_t imageIdentity = MRT_TestElfUnloadImageIdentity(reinterpret_cast<uintptr_t>(marker));
    bool loadCatalog = packagesLoaded > packagesBefore && MRT_TestElfUnloadPackagePathPresent(plugin);
    bool loadType = typesLoaded > typesBefore && MRT_TestElfUnloadTypePresent(typeName);
    bool loadCode = imageIdentity != 0 &&
        MRT_TestElfUnloadAddressLinked(reinterpret_cast<uintptr_t>(marker)) &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));

    void* token = MRT_TestElfUnloadReaderEnter();
    if (token == nullptr) {
        std::fprintf(stderr, "reader allocation failed\n");
        return 2;
    }
    std::atomic<bool> closeDone { false };
    std::atomic<int> closeRc { -1 };
    std::thread closer([&]() {
        closeRc.store(UnloadCJLibrary(plugin), std::memory_order_release);
        closeDone.store(true, std::memory_order_release);
    });

    bool observedPending = WaitFor([]() { return MRT_TestElfUnloadPending(); });
    bool advancedWhileHeld = observedPending && WaitFor([&]() {
        return !MRT_TestElfUnloadPending() || closeDone.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));
    bool readerHeldClose = observedPending && !advancedWhileHeld;
    MRT_TestElfUnloadReaderExit(token);
    closer.join();

    size_t rootsAfter = MRT_TestElfUnloadStaticRootCount();
    bool postCatalog = MRT_TestElfUnloadPackageIndexSize() == packagesBefore;
    bool postRoots = rootsLoaded > rootsBefore && rootsAfter == rootsBefore;
    bool postTypes = loadType && MRT_TestElfUnloadTypeIndexSize() == typesBefore;
    bool postCode = !MRT_TestElfUnloadAddressLinked(reinterpret_cast<uintptr_t>(marker)) &&
        !MRT_TestElfUnloadImageIdentityLinked(imageIdentity) &&
        !MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));

    bool readerUse = loadCode && readerHeldClose && closeRc.load(std::memory_order_acquire) == E_OK && postCode;
    bool directory = loadCatalog && postCatalog;
    bool typeIndex = postTypes;
    bool staticRoots = postRoots;
    Result("ElfUnload.ReaderUse", readerUse);
    Result("ElfUnload.Directory", directory);
    Result("ElfUnload.TypeIndex", typeIndex);
    Result("ElfUnload.StaticRoots", staticRoots);
    std::printf("DETAIL pending=%d close_while_held=%d close_rc=%d roots=%zu/%zu/%zu "
                "types=%zu/%zu/%zu packages=%zu/%zu/%zu\n",
                observedPending, advancedWhileHeld, closeRc.load(), rootsBefore, rootsLoaded,
                rootsAfter, typesBefore, typesLoaded,
                MRT_TestElfUnloadTypeIndexSize(), packagesBefore, packagesLoaded,
                MRT_TestElfUnloadPackageIndexSize());
    std::fflush(nullptr);

    int rc = 0;
    rc |= readerUse ? 0 : 1;
    rc |= directory ? 0 : 2;
    rc |= typeIndex ? 0 : 4;
    rc |= staticRoots ? 0 : 8;
    if (FiniCJRuntime() != E_OK) {
        rc |= 16;
    }
    return rc;
}

int RunDirectCallbackOrder(const char* plugin, const char* markerSymbol)
{
    if (!InitRuntime()) {
        std::fprintf(stderr, "runtime initialization failed\n");
        return 2;
    }
    MRT_TestElfUnloadResetDirectOrder();
    void* handle = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    void* marker = handle == nullptr ? nullptr : dlsym(handle, markerSymbol);
    int closeRc = handle == nullptr ? -1 : dlclose(handle);
    bool passed = handle != nullptr && marker != nullptr && closeRc == 0 &&
        MRT_TestElfUnloadDirectOrderValid();
    Result("ElfUnload.DirectCallbackOrder", passed);
    std::printf("DETAIL loaded=%d marker=%d close_rc=%d order=%d\n",
                handle != nullptr, marker != nullptr, closeRc, MRT_TestElfUnloadDirectOrderValid());
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunDirectQueuedProtection(const char* plugin, const char* queuedSymbol,
                              const char* blockerPlugin, const char* blockerSymbol)
{
    if (!InitRuntime(1) || LoadCJLibraryWithInit(blockerPlugin) != E_OK ||
        LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or direct queue probe initialization failed\n");
        return 2;
    }
    auto blocker = reinterpret_cast<CJTaskFunc>(FindCJSymbol(blockerPlugin, blockerSymbol));
    auto queued = reinterpret_cast<CJTaskFunc>(FindCJSymbol(plugin, queuedSymbol));
    void* handle = MRT_TestElfUnloadLibraryHandle(plugin);
    nativeEntered.store(false, std::memory_order_release);
    nativeRelease.store(false, std::memory_order_release);
    CJThreadHandle blockerTask = blocker == nullptr ? nullptr : RunCJTask(blocker, nullptr);
    bool blockerEntered = blockerTask != nullptr &&
        WaitFor([]() { return nativeEntered.load(std::memory_order_acquire); });
    CJThreadHandle queuedTask = blockerEntered && queued != nullptr ? RunCJTask(queued, nullptr) : nullptr;

    MRT_TestElfUnloadResetDirectOrder();
    std::atomic<bool> closeDone { false };
    std::atomic<int> closeRc { -1 };
    std::thread closer([&]() {
        closeRc.store(handle == nullptr ? -1 : dlclose(handle), std::memory_order_release);
        closeDone.store(true, std::memory_order_release);
    });
    bool preflightEntered = queuedTask != nullptr &&
        WaitFor([]() { return MRT_TestElfUnloadDirectPreflightEntered(); });
    bool advancedWhileQueued = preflightEntered && WaitFor([&]() {
        return closeDone.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));

    if (advancedWhileQueued) {
        Result("ElfUnload.DirectQueuedProtection", false);
        std::printf("DETAIL preflight=%d advanced=1\n", preflightEntered);
        std::fflush(nullptr);
        // The target entry is still queued; do not dispatch it after the direct
        // close has already completed in a deliberately disconnected build.
        std::_Exit(1);
    }

    nativeRelease.store(true, std::memory_order_release);
    void* blockerResult = nullptr;
    void* queuedResult = nullptr;
    bool blockerReturned = blockerTask != nullptr && GetTaskRet(blockerTask, &blockerResult) == E_OK;
    bool queuedReturned = queuedTask != nullptr && GetTaskRet(queuedTask, &queuedResult) == E_OK;
    if (blockerTask != nullptr) {
        ReleaseHandle(blockerTask);
    }
    if (queuedTask != nullptr) {
        ReleaseHandle(queuedTask);
    }
    closer.join();
    int blockerUnload = UnloadCJLibrary(blockerPlugin);
    bool passed = blockerEntered && preflightEntered && !advancedWhileQueued && blockerReturned &&
        queuedReturned && closeRc.load(std::memory_order_acquire) == 0 &&
        MRT_TestElfUnloadDirectOrderValid() && blockerUnload == E_OK;
    Result("ElfUnload.DirectQueuedProtection", passed);
    std::printf("DETAIL blocker_entered=%d preflight=%d advanced=%d blocker_returned=%d "
                "queued_returned=%d close_rc=%d order=%d blocker_unload=%d\n",
                blockerEntered, preflightEntered, advancedWhileQueued, blockerReturned, queuedReturned,
                closeRc.load(), MRT_TestElfUnloadDirectOrderValid(), blockerUnload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunDirectActiveProtection(const char* plugin, const char* activeSymbol)
{
    if (!InitRuntime() || LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or direct active probe initialization failed\n");
        return 2;
    }
    auto startActive = reinterpret_cast<CJTaskFunc>(FindCJSymbol(plugin, activeSymbol));
    void* handle = MRT_TestElfUnloadLibraryHandle(plugin);
    nativeEntered.store(false, std::memory_order_release);
    nativeRelease.store(false, std::memory_order_release);
    CJThreadHandle starterTask = startActive == nullptr ? nullptr : RunCJTask(startActive, nullptr);
    void* starterResult = nullptr;
    bool starterReturned = starterTask != nullptr && GetTaskRet(starterTask, &starterResult) == E_OK;
    if (starterTask != nullptr) {
        ReleaseHandle(starterTask);
    }
    bool entered = starterReturned &&
        WaitFor([]() { return nativeEntered.load(std::memory_order_acquire); });

    MRT_TestElfUnloadResetDirectOrder();
    std::atomic<bool> closeDone { false };
    std::atomic<int> closeRc { -1 };
    std::thread closer([&]() {
        closeRc.store(handle == nullptr ? -1 : dlclose(handle), std::memory_order_release);
        closeDone.store(true, std::memory_order_release);
    });
    bool preflightEntered = entered && WaitFor([]() { return MRT_TestElfUnloadDirectPreflightEntered(); });
    bool advancedWhileActive = preflightEntered && WaitFor([&]() {
        return closeDone.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));

    if (advancedWhileActive) {
        Result("ElfUnload.DirectActiveProtection", false);
        std::printf("DETAIL preflight=%d advanced=1\n", preflightEntered);
        std::fflush(nullptr);
        // The managed caller is still returning through this image; stop the
        // disconnected arm before releasing that call.
        std::_Exit(1);
    }

    nativeRelease.store(true, std::memory_order_release);
    closer.join();
    bool passed = startActive != nullptr && starterReturned && entered && preflightEntered &&
        !advancedWhileActive && closeRc.load(std::memory_order_acquire) == 0 &&
        MRT_TestElfUnloadDirectOrderValid();
    Result("ElfUnload.DirectActiveProtection", passed);
    std::printf("DETAIL starter_returned=%d entered=%d preflight=%d advanced=%d close_rc=%d order=%d\n",
                starterReturned, entered, preflightEntered, advancedWhileActive, closeRc.load(),
                MRT_TestElfUnloadDirectOrderValid());
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunDirectUnrelatedStw(const char* plugin, const char* markerSymbol)
{
    if (!InitRuntime()) {
        std::fprintf(stderr, "runtime initialization failed\n");
        return 2;
    }
    void* handle = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    void* marker = handle == nullptr ? nullptr : dlsym(handle, markerSymbol);
    MRT_TestElfUnloadResetDirectOrder();
    MRT_TestElfUnloadResetUnrelatedStw();
    std::thread holder([]() { MRT_TestElfUnloadHoldUnrelatedStw(); });
    bool stwHeld = WaitFor([]() { return MRT_TestElfUnloadUnrelatedStwHeld(); });

    std::atomic<bool> closeDone { false };
    std::atomic<int> closeRc { -1 };
    std::thread closer([&]() {
        closeRc.store(handle == nullptr ? -1 : dlclose(handle), std::memory_order_release);
        closeDone.store(true, std::memory_order_release);
    });
    bool preflightEntered = stwHeld && WaitFor([]() { return MRT_TestElfUnloadDirectPreflightEntered(); });
    bool usedUnrelatedStw = preflightEntered && WaitFor([&]() {
        return closeDone.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));

    MRT_TestElfUnloadReleaseUnrelatedStw();
    holder.join();
    closer.join();
    bool passed = handle != nullptr && marker != nullptr && stwHeld && preflightEntered &&
        !usedUnrelatedStw && closeRc.load(std::memory_order_acquire) == 0 &&
        MRT_TestElfUnloadDirectOrderValid();
    Result("ElfUnload.DirectUnrelatedStw", passed);
    std::printf("DETAIL loaded=%d marker=%d stw_held=%d preflight=%d used_unrelated=%d "
                "close_rc=%d order=%d\n",
                handle != nullptr, marker != nullptr, stwHeld, preflightEntered, usedUnrelatedStw,
                closeRc.load(), MRT_TestElfUnloadDirectOrderValid());
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunEarlyReturnNoPurge(const char* plugin, const char* markerSymbol)
{
    if (!InitRuntime() || LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or early return probe initialization failed\n");
        return 2;
    }
    void* marker = FindCJSymbol(plugin, markerSymbol);
    int nullRc = UnloadCJLibrary(nullptr);
    int missingRc = UnloadCJLibrary("libelfunload-missing.so");
    bool retained = marker != nullptr &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));
    int finalUnload = UnloadCJLibrary(plugin);
    bool passed = nullRc != E_OK && missingRc != E_OK && retained && finalUnload == E_OK;
    Result("ElfUnload.EarlyReturnNoPurge", passed);
    std::printf("DETAIL null_rc=%d missing_rc=%d retained=%d final_unload=%d\n",
                nullRc, missingRc, retained, finalUnload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunCompatiblePreInitializeNoPurge(const char* plugin, const char* markerSymbol)
{
    if (!InitRuntime() || LoadCJLibraryWithInit(plugin) != E_OK) {
        std::fprintf(stderr, "runtime or compatible pre-initialize probe initialization failed\n");
        return 2;
    }
    void* handle = MRT_TestElfUnloadLibraryHandle(plugin);
    void* metadata = handle == nullptr ? nullptr : dlsym(handle, "_CJMetadataStart");
    void* marker = FindCJSymbol(plugin, markerSymbol);
    bool before = metadata != nullptr && marker != nullptr &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));
    if (metadata != nullptr) {
        MRT_PreInitializePackage(reinterpret_cast<uint64_t>(metadata));
    }
    bool retained = marker != nullptr &&
        MRT_TestElfUnloadFuncDescPresent(reinterpret_cast<uintptr_t>(marker));
    int unload = UnloadCJLibrary(plugin);
    bool passed = before && retained && unload == E_OK;
    Result("ElfUnload.CompatiblePreInitializeNoPurge", passed);
    std::printf("DETAIL metadata=%d before=%d retained=%d unload=%d\n",
                metadata != nullptr, before, retained, unload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

int RunPackageInfoUse(const char* plugin)
{
    if (!InitRuntime()) {
        std::fprintf(stderr, "runtime or package snapshot probe initialization failed\n");
        return 2;
    }
    MRT_TestElfUnloadEnablePackageReaderPause();
    std::atomic<void*> packageInfo { nullptr };
    std::thread loader([&]() {
        packageInfo.store(CJ_MCC_LoadPackage(plugin), std::memory_order_release);
    });
    bool readerPaused = WaitFor([]() { return MRT_TestElfUnloadPackageReaderPaused(); });

    std::atomic<bool> unloadDone { false };
    std::atomic<int> unloadRc { -1 };
    std::thread unloader([&]() {
        unloadRc.store(UnloadCJLibrary(plugin), std::memory_order_release);
        unloadDone.store(true, std::memory_order_release);
    });
    bool writerPending = readerPaused && WaitFor([]() { return MRT_TestElfUnloadPending(); });
    bool advancedWhilePackageUsed = writerPending && WaitFor([&]() {
        return unloadDone.load(std::memory_order_acquire);
    }, std::chrono::milliseconds(250));
    if (advancedWhilePackageUsed) {
        Result("ElfUnload.PackageInfoUse", false);
        std::printf("DETAIL reader_paused=%d writer_pending=%d advanced=1 unload_rc=%d\n",
                    readerPaused, writerPending, unloadRc.load());
        std::fflush(nullptr);
        // The deliberately disconnected arm may have removed the image before
        // the snapshot callback copies its fields. Do not resume that callback.
        std::_Exit(1);
    }

    MRT_TestElfUnloadReleasePackageReaderPause();
    loader.join();
    unloader.join();
    void* snapshot = packageInfo.load(std::memory_order_acquire);
    const char* name = reinterpret_cast<uintptr_t>(snapshot) > 3 ?
        CJ_MCC_GetPackageName(snapshot) : nullptr;
    const char* version = name == nullptr ? nullptr : CJ_MCC_GetPackageVersion(snapshot);
    uint32_t typeCount = version == nullptr ? 0 : CJ_MCC_GetPackageNumOfTypeInfos(snapshot);
    bool passed = readerPaused && writerPending && !advancedWhilePackageUsed &&
        unloadRc.load(std::memory_order_acquire) == E_OK && name != nullptr && *name != '\0' &&
        version != nullptr && typeCount != 0;
    Result("ElfUnload.PackageInfoUse", passed);
    std::printf("DETAIL reader_paused=%d writer_pending=%d advanced=%d unload_rc=%d "
                "snapshot=%d name=%s version=%s types=%u\n",
                readerPaused, writerPending, advancedWhilePackageUsed, unloadRc.load(), snapshot != nullptr,
                name == nullptr ? "<null>" : name, version == nullptr ? "<null>" : version, typeCount);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 10) {
        std::fprintf(stderr,
                     "usage: %s MODE PLUGIN MARKER_SYMBOL NATIVE_BLOCK_SYMBOL QUEUED_TASK_SYMBOL "
                     "SPAWN_ACTIVE_SYMBOL TYPE_NAME "
                     "BLOCKER_PLUGIN BLOCKER_SYMBOL\n",
                     argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "queued") == 0) {
        return RunQueuedTaskReject(argv[2], argv[5], argv[8], argv[9]);
    }
    if (std::strcmp(argv[1], "gc") == 0) {
        return RunGcReaderDrain(argv[2], argv[4]);
    }
    if (std::strcmp(argv[1], "native") == 0) {
        return RunNativeFrameReject(argv[2], argv[3], argv[6]);
    }
    if (std::strcmp(argv[1], "core") == 0) {
        return RunCoreUnload(argv[2], argv[3], argv[7]);
    }
    if (std::strcmp(argv[1], "direct") == 0) {
        return RunDirectCallbackOrder(argv[2], argv[3]);
    }
    if (std::strcmp(argv[1], "direct_queued") == 0) {
        return RunDirectQueuedProtection(argv[2], argv[5], argv[8], argv[9]);
    }
    if (std::strcmp(argv[1], "direct_active") == 0) {
        return RunDirectActiveProtection(argv[2], argv[6]);
    }
    if (std::strcmp(argv[1], "direct_stw") == 0) {
        return RunDirectUnrelatedStw(argv[2], argv[3]);
    }
    if (std::strcmp(argv[1], "early") == 0) {
        return RunEarlyReturnNoPurge(argv[2], argv[3]);
    }
    if (std::strcmp(argv[1], "preinit") == 0) {
        return RunCompatiblePreInitializeNoPurge(argv[2], argv[3]);
    }
    if (std::strcmp(argv[1], "package") == 0) {
        return RunPackageInfoUse(argv[2]);
    }
    std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
