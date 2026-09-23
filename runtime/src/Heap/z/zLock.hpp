// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zLock.hpp:31-39: ZLock owns its platform mutex.
#pragma once
#include <condition_variable>
#include <mutex>

namespace MapleRuntime {
class ZLock {
private:
    std::mutex _lock;

public:
    void lock();
    bool try_lock();
    void unlock();
};

class ZConditionLock {
private:
    std::mutex _lock;
    std::condition_variable _cv;

public:
    void lock() { _lock.lock(); }
    bool try_lock() { return _lock.try_lock(); }
    void unlock() { _lock.unlock(); }

    // zLock.hpp:60-67: wait/notify on the held lock.
    bool wait(uint64_t millis = 0);
    void notify() { _cv.notify_one(); }
    void notify_all() { _cv.notify_all(); }
};

template <typename T>
class ZLocker {
private:
    T* const _lock;

public:
    ZLocker(T* lock);
    ~ZLocker();

    ZLocker(const ZLocker&) = delete;
    ZLocker& operator=(const ZLocker&) = delete;
};
} // namespace MapleRuntime
