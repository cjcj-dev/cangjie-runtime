// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_worker_fixture.hpp"
#include "Heap/z/zGCIdPrinter.hpp"
#include <atomic>
#include <functional>
#include "Base/Semaphore.h"
#define private public
#include "Heap/z/workerThread.hpp"
#undef private
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGlobals.hpp"
namespace MapleRuntime { namespace GcUnit {
WorkerBudgetFixture::WorkerBudgetFixture(uint32_t count)
    : young(ZYoungGCThreads), old(ZOldGCThreads)
{
    ZYoungGCThreads = count;
    ZOldGCThreads = count;
}
WorkerBudgetFixture::~WorkerBudgetFixture()
{
    ZYoungGCThreads = young;
    ZOldGCThreads = old;
}
void InitializeGenerationWorkers(ZGeneration& generation, uint32_t count)
{
    // Keep the configured maximum consistent with the live generation pool.
    // Later product resize decisions read this same configuration.
    if (generation.id() == ZGenerationId::young) ZYoungGCThreads = count;
    else ZOldGCThreads = count;
    generation.InitializeWorkers();
}
WorkerFixture::WorkerFixture(uint32_t id) : saved(WorkerThread::worker_id())
{
    WorkerThread::set_worker_id(id);
}
WorkerFixture::~WorkerFixture() { WorkerThread::set_worker_id(saved); }
} }
