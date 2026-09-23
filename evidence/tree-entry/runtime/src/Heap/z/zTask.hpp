// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_Z_ZTASK_HPP
#define MRT_GC_Z_ZTASK_HPP

#include <cstdint>

#include "Heap/z/workerThread.hpp"

namespace MapleRuntime {
// zTask.hpp:30-51. A ZTask embeds the WorkerTask adapter; work() takes no
// argument and reads WorkerThread::worker_id() where a worker index is needed.
class ZTask {
private:
    class Task : public WorkerTask {
    private:
        ZTask* const _task;

    public:
        Task(ZTask* task, const char* name);

        virtual void work(uint32_t worker_id);
    };

    Task _worker_task;

public:
    explicit ZTask(const char* name);
    virtual ~ZTask() = default;

    const char* name() const;
    WorkerTask* worker_task();

    virtual void work() = 0;
};

// zTask.hpp:53-57
class ZRestartableTask : public ZTask {
public:
    explicit ZRestartableTask(const char* name);
    virtual void resize_workers(uint32_t nworkers);
};
} // namespace MapleRuntime
#endif // MRT_GC_Z_ZTASK_HPP
