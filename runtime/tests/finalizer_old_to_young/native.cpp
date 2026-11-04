// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdio>
#include "HeapManager.inline.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zDriver.hpp"

// Test executable bridge only. No runtime export or product hook is added.
extern "C" int64_t R11CollectYoung()
{
    using namespace MapleRuntime;
    uint32_t oldBefore, youngBefore, oldAfter, youngAfter;
    {
        DriverLocker lock;
        oldBefore = ZGeneration::old()->seqnum();
        youngBefore = ZGeneration::young()->seqnum();
    }
    HeapManager::RequestGC(GC_REASON_YOUNG);
    {
        DriverLocker lock;
        oldAfter = ZGeneration::old()->seqnum();
        youngAfter = ZGeneration::young()->seqnum();
    }
    std::fprintf(stderr, "R11_CYCLE old=%u/%u young=%u/%u\n",
                 oldBefore, oldAfter, youngBefore, youngAfter);
    return oldBefore == oldAfter && youngAfter > youngBefore ? 0 : 12;
}
