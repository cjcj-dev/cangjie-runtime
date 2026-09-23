// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zDeferredConstructed.inline.hpp:25-87
#pragma once
#include "Heap/z/zDeferredConstructed.hpp"

#include <cassert>
#include <new>
#include <type_traits>

namespace MapleRuntime {
template <typename T>
inline ZDeferredConstructed<T>::ZDeferredConstructed()
    : _initialized(false)
{
    // Do not construct value immediately. Value is constructed at a later point
    // in time using initialize().
}

template <typename T>
inline ZDeferredConstructed<T>::~ZDeferredConstructed()
{
    assert(_initialized && "must be initialized before being destructed");
    _t.~T();
}

template <typename T>
inline T* ZDeferredConstructed<T>::get()
{
    assert(_initialized && "must be initialized before access");
    return &_t;
}

template <typename T>
inline const T* ZDeferredConstructed<T>::get() const
{
    assert(_initialized && "must be initialized before access");
    return &_t;
}

template <typename T>
inline T& ZDeferredConstructed<T>::operator*()
{
    return *get();
}

template <typename T>
inline const T& ZDeferredConstructed<T>::operator*() const
{
    return *get();
}

template <typename T>
inline T* ZDeferredConstructed<T>::operator->()
{
    return get();
}

template <typename T>
inline const T* ZDeferredConstructed<T>::operator->() const
{
    return get();
}

template <typename T>
template <typename... Ts>
inline void ZDeferredConstructed<T>::initialize(Ts&&... args)
{
    assert(!_initialized && "Double initialization forbidden");
    _initialized = true;
    using NCVP = std::add_pointer_t<std::remove_cv_t<T>>;
    ::new (const_cast<NCVP>(get())) T(args...);
}
} // namespace MapleRuntime
