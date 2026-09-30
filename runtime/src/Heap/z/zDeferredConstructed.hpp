// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zDeferredConstructed.hpp:25-57
#pragma once

namespace MapleRuntime {
template <typename T>
class ZDeferredConstructed {
    union {
        T _t;
    };

#ifndef NDEBUG
    bool _initialized;
#endif

    ZDeferredConstructed(const ZDeferredConstructed&) = delete;
    ZDeferredConstructed& operator=(const ZDeferredConstructed&) = delete;

public:
    ZDeferredConstructed();
    ~ZDeferredConstructed();

    T* get();
    const T* get() const;

    T& operator*();
    const T& operator*() const;

    T* operator->();
    const T* operator->() const;

    template <typename... Ts>
    void initialize(Ts&&... args);
};
} // namespace MapleRuntime
