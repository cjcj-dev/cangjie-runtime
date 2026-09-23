// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zGeneration.hpp"
namespace MapleRuntime {
class ZGenerationTest {
public:
    static void SetTenuringThreshold(ZGenerationYoung& generation, uint32_t value)
    {
        generation._tenuring_threshold = value;
    }
};
}
