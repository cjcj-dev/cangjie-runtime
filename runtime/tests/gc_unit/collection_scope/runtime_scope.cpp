// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "CangjieRuntime.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include <cstdio>
#include <cstdlib>
using namespace MapleRuntime;

// Debugger completion boundary in the fixture, never a product replacement.
extern "C" __attribute__((noinline)) void CollectionScopeFixtureComplete() {}
int main()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.staticGCThreads = true;
    const int rc = InitCJRuntime(&params);
    std::printf("SCOPE_INIT_RC=%d\n", rc);
    if (rc != E_OK) return 2;
    // Input from a preceding accounting interval, consumed by mark-start reset.
    Heap::GetHeap().old().increase_freed(37);
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    Heap::GetHeap().RequestGC(GC_REASON_BACKUP, false);
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    CollectionScopeFixtureComplete();
    std::fflush(nullptr);
    std::_Exit(0);
}
