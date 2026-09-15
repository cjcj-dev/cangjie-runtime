// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zTask.cpp:24-48
#include "Heap/z/zTask.hpp"

namespace MapleRuntime {
ZTask::Task::Task(ZTask* task, const char* name)
    : WorkerTask(name),
      _task(task) {}

void ZTask::Task::work(uint32_t worker_id)
{
    _task->work();
}

ZTask::ZTask(const char* name)
    : _worker_task(this, name) {}

const char* ZTask::name() const
{
    return _worker_task.name();
}

WorkerTask* ZTask::worker_task()
{
    return &_worker_task;
}

ZRestartableTask::ZRestartableTask(const char* name)
    : ZTask(name) {}

void ZRestartableTask::resize_workers(uint32_t nworkers) {}
} // namespace MapleRuntime
