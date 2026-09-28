// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real runtime/task/GC entrance for signals_posix.cpp:642-654 regression.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "SignalManager.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C" void CJ_MCC_AddSignalHandler(int, SignalAction*);
static std::atomic<int> delivered{0};
static bool RecordSignal(int sig, siginfo_t*, void*)
{
    delivered.store(sig, std::memory_order_release);
    return true;
}

extern "C" __attribute__((noinline)) void* SignalWatermarkWork(void*)
{
    MapleRuntime::Heap::GetHeap().RequestGC(MapleRuntime::GC_REASON_USER);
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    const int sig = std::atoi(argv[2]);
    RuntimeParam param{};
    param.heapParam.heapSize = 64 * 1024;
    param.coParam.processorNum = 1;
    if (InitCJRuntime(&param) != E_OK) { return 3; }
    SignalAction action{};
    action.saSignalAction = RecordSignal;
    action.scFlags = SA_SIGINFO;
    sigemptyset(&action.scMask);
    CJ_MCC_AddSignalHandler(sig, &action);
    std::fprintf(stderr, "SIGNAL_INPUT registered=%d mode=%s\n", sig, argv[1]);
    if (std::strcmp(argv[1], "watermark") == 0) {
        auto task = RunCJTask(SignalWatermarkWork, nullptr);
        void* result = nullptr;
        if (task == nullptr || GetTaskRet(task, &result) != E_OK) { return 4; }
        ReleaseHandle(task);
    } else {
        std::raise(sig);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (delivered.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const int observed = delivered.load(std::memory_order_acquire);
        std::fprintf(stderr, "SIGNAL_NORMAL_TARGET executed=1 signal=%d observed=%d\n", sig, observed);
        std::_Exit(observed == sig ? 0 : 1);
    }
    std::_Exit(0);
}
