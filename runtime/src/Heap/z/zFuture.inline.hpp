// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zFuture.inline.hpp:24-59
#pragma once
#include "Heap/z/zFuture.hpp"
#include "Base/Semaphore.inline.h"

namespace MapleRuntime {
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
    // ZGC zFuture.inline.hpp:46-52: route by the current thread's identity.
    Mutator* const thread = Mutator::GetMutator();
    if (thread != nullptr && ThreadLocal::GetThreadType() != ThreadType::GC_THREAD) {
        _sema.wait_with_safepoint_check();
    } else {
        _sema.wait();
    }

    // Return value
    return _value;
}
} // namespace MapleRuntime
