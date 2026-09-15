// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/shared/workerThread.hpp — the task and worker-id contract consumed by
// gc/z (ZTask adapter zTask.hpp:32-40; ZPerWorkerStorage::id
// zValue.inline.hpp:112-114; ZMark zMark.cpp:456,610). The dispatcher and
// thread pool halves (WorkerTaskDispatcher/WorkerThreads, workerThread.hpp:57-135)
// belong to the worker-pool package (P07).
#pragma once
#include <cstdint>

#include "Heap/z/zGCIdPrinter.hpp"

namespace MapleRuntime {
// gc/shared/workerThread.hpp:44-56. An task to be worked on by worker threads
class WorkerTask {
private:
    const char* _name;
    const uint64_t _gc_id;

public:
    explicit WorkerTask(const char* name) :
        _name(name),
        _gc_id(GCIdMark::Current()) {}
    virtual ~WorkerTask() = default;

    const char* name() const { return _name; }
    uint64_t gc_id() const { return _gc_id; }

    virtual void work(uint32_t worker_id) = 0;
};

// gc/shared/workerThread.hpp:132-155 (worker id half). The id is THREAD_LOCAL:
// the dispatcher sets it before task->work(worker_id) (workerThread.cpp:68-69)
// and every reader takes it from the thread, never from a task parameter.
class WorkerThread {
    friend class WorkerTaskDispatcher;
    friend class GCWorkers;
    friend class RuntimeWorkers;

private:
    static thread_local uint32_t _worker_id;

    static void set_worker_id(uint32_t worker_id) { _worker_id = worker_id; }

public:
    static uint32_t worker_id() { return _worker_id; }
};
} // namespace MapleRuntime
