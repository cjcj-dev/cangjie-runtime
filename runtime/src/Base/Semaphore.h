// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include <condition_variable>
#include <mutex>

namespace MapleRuntime {
// runtime/semaphore.hpp Semaphore: a counting semaphore over the platform
// mutex/condvar (I14, PLAN §5: same-layer primitives).
class Semaphore {
private:
    std::mutex _mutex;
    std::condition_variable _cv;
    unsigned _count;

public:
    explicit Semaphore(unsigned value = 0) : _count(value) {}

    void signal(unsigned count = 1);
    void wait();
    void wait_with_safepoint_check();
    bool trywait();
};

inline void Semaphore::signal(unsigned count)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _count += count;
    }
    if (count == 1) {
        _cv.notify_one();
    } else {
        _cv.notify_all();
    }
}

inline void Semaphore::wait()
{
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this] { return _count > 0; });
    --_count;
}

inline bool Semaphore::trywait()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_count == 0) {
        return false;
    }
    --_count;
    return true;
}

} // namespace MapleRuntime
