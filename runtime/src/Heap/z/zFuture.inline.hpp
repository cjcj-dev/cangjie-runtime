// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zFuture.inline.hpp:24-59
#pragma once
#include "Heap/z/zFuture.hpp"

namespace MapleRuntime {
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

template <typename T>
inline ZFuture<T>::ZFuture()
    : _value() {}

template <typename T>
inline void ZFuture<T>::set(T value)
{
    // Set value
    _value = value;

    // Notify waiter
    _sema.signal();
}

template <typename T>
inline T ZFuture<T>::get()
{
    // Wait for notification. zFuture.inline.hpp:47-53 branches on Java thread
    // to wait_with_safepoint_check; here the mutator side enters its
    // saferegion at the call site (ScopedEnterSaferegion, I3/I4 PLAN §5).
    _sema.wait();

    // Return value
    return _value;
}
} // namespace MapleRuntime
