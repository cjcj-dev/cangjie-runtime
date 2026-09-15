// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zSafeDelete.hpp:24-48
#pragma once
#include <type_traits>

#include "Heap/z/zArray.hpp"

namespace MapleRuntime {
template <typename T>
class ZSafeDelete {
private:
    using ItemT = std::remove_extent_t<T>;

    ZActivatedArray<T> _deferred;

    static void immediate_delete(ItemT* item);

public:
    explicit ZSafeDelete(bool locked = true);

    void enable_deferred_delete();
    void disable_deferred_delete();

    void schedule_delete(ItemT* item);
};
} // namespace MapleRuntime
