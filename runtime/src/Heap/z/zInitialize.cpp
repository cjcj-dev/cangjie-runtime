// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zLargePages.hpp"
namespace MapleRuntime {
// ZGC zInitialize.cpp:61-66: establish the address contract before heap
// reservation, then the large page state the backing file depends on.
void ZInitialize::initialize()
{
    ZGlobalsPointers::initialize();
    ZLargePages::initialize();
}
}
