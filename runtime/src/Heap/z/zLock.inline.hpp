// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zLock.inline.hpp:64-115
#pragma once
#include "Heap/z/zLock.hpp"

#include <chrono>

namespace MapleRuntime {
// gc/z/zLock.inline.hpp:33-42
inline void ZLock::lock()
{
    _lock.lock();
}

inline bool ZLock::try_lock()
{
    return _lock.try_lock();
}

inline void ZLock::unlock()
{
    _lock.unlock();
}

// zLock.inline.hpp:76-78 (PlatformMonitor::wait on the held lock; 0 == forever)
inline bool ZConditionLock::wait(uint64_t millis)
{
    std::unique_lock<std::mutex> held(_lock, std::adopt_lock);
    bool timed_out = false;
    if (millis == 0) {
        _cv.wait(held);
    } else {
        timed_out = _cv.wait_for(held, std::chrono::milliseconds(millis)) == std::cv_status::timeout;
    }
    held.release();
    return timed_out;
}

template <typename T>
inline ZLocker<T>::ZLocker(T* lock)
    : _lock(lock)
{
    if (_lock != nullptr) {
        _lock->lock();
    }
}

template <typename T>
inline ZLocker<T>::~ZLocker()
{
    if (_lock != nullptr) {
        _lock->unlock();
    }
}
} // namespace MapleRuntime
