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
        (FindCJSymbol(plugin, markerSymbol) != nullptr);
    std::printf("ACTIVE_IMAGE_TARGET executed=1 entered=%d first_unload=%d rejected=%d\n",
                entered, firstUnload, rejected);
    if (!rejected) {
        Result("ElfUnload.NativeFrameRejectPublic", false);
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
        !(FindCJSymbol(plugin, markerSymbol) != nullptr);
    Result("ElfUnload.NativeFrameRejectPublic", passed);
    std::printf("DETAIL starter_returned=%d entered=%d first_unload=%d second_unload=%d\n",
                starterReturned, entered, firstUnload, secondUnload);
    int rc = passed ? 0 : 1;
    if (FiniCJRuntime() != E_OK) {
        rc |= 2;
    }
    return rc;
}

} // namespace
int main(int argc, char** argv) {
    if (argc != 4) { return 2; }
    return RunNativeFrameReject(argv[1], argv[2], argv[3]);
}
