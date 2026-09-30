// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include <cstdint>
namespace MapleRuntime {
class ZGeneration;
namespace GcUnit {
void InitializeGenerationWorkers(ZGeneration& generation, uint32_t count);
// Configure the same generation budgets consumed by the product constructor.
class WorkerBudgetFixture {
    uint32_t young;
    uint32_t old;
public:
    explicit WorkerBudgetFixture(uint32_t count);
    ~WorkerBudgetFixture();
};
// Supply the thread identity normally installed by the product dispatcher.
// gc_unit_main initializes the maximum before any per-worker allocation.
class WorkerFixture {
    uint32_t saved;
public:
    explicit WorkerFixture(uint32_t id = 0);
    ~WorkerFixture();
};
} }
