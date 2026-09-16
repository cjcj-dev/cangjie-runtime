// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zInitialize.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zLargePages.hpp"
namespace MapleRuntime {
void ZInitialize::initialize()
{
    ZGlobalsPointers::initialize();
    ZCPU::initialize();
    ZLargePages::initialize();
    ZHeuristics::set_medium_page_size();
}
}
