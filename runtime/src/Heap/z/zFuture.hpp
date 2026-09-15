// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zFuture.hpp:24-43
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
    bool trywait();
};

template <typename T>
class ZFuture {
private:
    Semaphore _sema;
    T         _value;

public:
    ZFuture();

    void set(T value);
    T get();
};
} // namespace MapleRuntime
