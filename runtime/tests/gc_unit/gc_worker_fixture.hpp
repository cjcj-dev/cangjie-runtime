// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include <cstdint>
namespace MapleRuntime { namespace GcUnit {
// Supply the thread identity normally installed by the product dispatcher.
// gc_unit_main initializes the maximum before any per-worker allocation.
class WorkerFixture {
    uint32_t saved;
public:
    explicit WorkerFixture(uint32_t id = 0);
    ~WorkerFixture();
};
} }
