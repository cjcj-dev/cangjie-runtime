// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zLargePages.hpp"
namespace MapleRuntime {
// ZGC zInitialize.cpp:61-66: address contract, CPU storage, large-page state.
void ZInitialize::initialize()
{
    ZGlobalsPointers::initialize();
    ZCPU::initialize();
    ZLargePages::initialize();
}
}
