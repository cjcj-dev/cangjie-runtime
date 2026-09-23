// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zIndexDistributor.hpp:24-50
#pragma once
#include <cstddef>

namespace MapleRuntime {
class ZIndexDistributor {
private:
    void* _strategy;

    template <typename Strategy>
    Strategy* strategy();

    static void* create_strategy(int count);

public:
    ZIndexDistributor(int count);
    ~ZIndexDistributor();

    template <typename Function>
    void do_indices(Function function);

    // Returns a count that is max_count or larger and upholds the requirements
    // for using the ZIndexDistributor strategy specfied by ZIndexDistributorStrategy
    static size_t get_count(size_t max_count);
};
} // namespace MapleRuntime
