// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zSafeDelete.inline.hpp:24-62
#pragma once
#include "Heap/z/zSafeDelete.hpp"

#include <type_traits>

#include "Heap/z/zArray.inline.hpp"

namespace MapleRuntime {
template <typename T>
ZSafeDelete<T>::ZSafeDelete(bool locked)
    : _deferred(locked) {}

template <typename T>
void ZSafeDelete<T>::immediate_delete(ItemT* item)
{
    if (std::is_array<T>::value) {
        delete [] item;
    } else {
        delete item;
    }
}

template <typename T>
void ZSafeDelete<T>::enable_deferred_delete()
{
    _deferred.activate();
}

template <typename T>
void ZSafeDelete<T>::disable_deferred_delete()
{
    _deferred.deactivate_and_apply(immediate_delete);
}

template <typename T>
void ZSafeDelete<T>::schedule_delete(ItemT* item)
{
    if (!_deferred.add_if_activated(item)) {
        immediate_delete(item);
    }
}
} // namespace MapleRuntime
