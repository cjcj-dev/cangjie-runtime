// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zGCIdPrinter.hpp"
#define private public
#include "Heap/z/workerThread.hpp"
#undef private
#include "Heap/z/zGlobals.hpp"

namespace MapleRuntime { namespace GcUnit {
// Standalone algorithm tests supply the worker environment normally installed
// by GCThread::Init and GCWorkers::WorkerLoop. Storage capacity stays fixed
// across all logical worker counts exercised in one process.
class WorkerFixture {
    uint32_t saved;
public:
    explicit WorkerFixture(uint32_t id = 0) : saved(WorkerThread::worker_id())
    {
        static const bool initialized = [] { ConcGCThreads = 64; return true; }();
        (void)initialized;
        WorkerThread::set_worker_id(id);
    }
    ~WorkerFixture() { WorkerThread::set_worker_id(saved); }
};
} } // namespace MapleRuntime::GcUnit
