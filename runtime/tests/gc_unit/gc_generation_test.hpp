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

// Locally constructed generations publish the same singleton slots as the
// runtime. Fixture lifetime bookkeeping belongs to the test, not the product.
namespace MapleRuntime {
class GenerationFixtureState : public ZGeneration {
public:
    class Scope {
    public:
        Scope() : savedYoung(ZGeneration::young()), savedOld(ZGeneration::old()) {}
        ~Scope()
        {
            _young = savedYoung;
            _old = savedOld;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        ZGenerationYoung* savedYoung;
        ZGenerationOld* savedOld;
    };
};
}
