// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_worker_fixture.hpp"
#include "Heap/z/zGCIdPrinter.hpp"
#define private public
#include "Heap/z/workerThread.hpp"
#undef private
namespace MapleRuntime { namespace GcUnit {
WorkerFixture::WorkerFixture(uint32_t id) : saved(WorkerThread::worker_id())
{
    WorkerThread::set_worker_id(id);
}
WorkerFixture::~WorkerFixture() { WorkerThread::set_worker_id(saved); }
} }
