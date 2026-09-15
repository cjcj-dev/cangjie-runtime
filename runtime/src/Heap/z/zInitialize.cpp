// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zCPU.hpp"
namespace MapleRuntime {
// ZGC zInitialize.cpp:61-62: establish the address contract before heap
// reservation, then the CPU affinity table before any per-CPU storage is used.
void ZInitialize::initialize()
{
    ZGlobalsPointers::initialize();
    ZCPU::initialize();
}
}
