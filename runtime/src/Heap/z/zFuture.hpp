// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zFuture.hpp:24-43
#pragma once
#include "Base/Semaphore.h"

namespace MapleRuntime {
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
