// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <vector>

#include "Base/LogFile.h"
#include "Base/Macros.h"

namespace MapleRuntime {
class GCWorkerTask {
public:
    virtual ~GCWorkerTask() = default;
    virtual void Work(uint32_t workerId) = 0;
};

class GCRestartableWorkerTask : public GCWorkerTask {
public:
    // Called only after every participant has returned its private work.
    virtual void ResizeWorkers(uint32_t workers) = 0;
};

}
