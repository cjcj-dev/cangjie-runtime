// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace MapleRuntime;

// Debugger observation of the same product state returned by Snapshot().
// This pointer owns no state and installs no callback in the runtime.
const ZGeneration* observed_young = nullptr;
int main(int argc, char** argv)
{
    RuntimeParam param{};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    if (InitCJRuntime(&param) != E_OK) { return 79; }
    observed_young = &Heap::GetHeap().young();
    const bool concurrent = argc == 2 && std::strcmp(argv[1], "concurrent") == 0;
    std::atomic<bool> start{false};
    std::thread young([&] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        if (concurrent) {
            for (unsigned i = 0; i < 100; ++i) {
                Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
            }
        }
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 50; ++i) { CJ_MRT_ForceFullGC(); }
    young.join();
    std::printf("OLD_VERIFY_REQUESTS_COMPLETED old=50 young=%u\n", concurrent ? 100u : 0u);
    return 0;
}
