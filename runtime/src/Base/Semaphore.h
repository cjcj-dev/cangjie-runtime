// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BASE_SEMAPHORE_H
#define MRT_BASE_SEMAPHORE_H

#include <cerrno>
#include <cstdint>
#include <semaphore.h>

#include "Base/Log.h"

namespace MapleRuntime {
// runtime/semaphore.hpp:33-58 + os/posix/semaphore_posix.cpp:30-70. The
// same-layer primitive behind HotSpot's Semaphore is sem_t; signal(count)
// posts count times and wait() retries an interrupted wait.
class Semaphore {
public:
    explicit Semaphore(uint32_t value = 0)
    {
        const int ret = ::sem_init(&sem, 0, value);
        CHECK_DETAIL(ret == 0, "sem_init failed: %d", errno);
    }
    ~Semaphore() { (void)::sem_destroy(&sem); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void signal(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i) {
            const int ret = ::sem_post(&sem);
            CHECK_DETAIL(ret == 0, "sem_post failed: %d", errno);
        }
    }

    void wait()
    {
        int ret;
        do {
            ret = ::sem_wait(&sem);
        } while (ret != 0 && errno == EINTR);
        CHECK_DETAIL(ret == 0, "sem_wait failed: %d", errno);
    }

private:
    sem_t sem;
};
} // namespace MapleRuntime
#endif // MRT_BASE_SEMAPHORE_H
