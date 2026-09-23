// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_Z_RUNTIME_WORKERS_HPP
#define MRT_Z_RUNTIME_WORKERS_HPP

#include "Heap/z/workerThread.hpp"

namespace MapleRuntime {
// zRuntimeWorkers.hpp:34-45: runtime workers are distinct from GC generations.
class ZRuntimeWorkers {
    WorkerThreads _workers;
public:
    ZRuntimeWorkers();
    WorkerThreads* workers() { return &_workers; }
    void stop() { _workers.stop(); }
    void threads_do(const std::function<void(WorkerThread*)>& visitor) const
    {
        _workers.threads_do(visitor);
    }
};
}
#endif
