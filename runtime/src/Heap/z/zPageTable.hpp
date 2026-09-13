// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zGranuleMap.hpp"
#include "Heap/z/zIndexDistributor.hpp"
namespace MapleRuntime {
// zPageTable.inline.hpp:79-99. The map includes reservation holes. A page
// spanning several granules is emitted only at its own start granule.
// z_globals.hpp:99 selects claim-tree by default. The diagnostic strategy
// selector is not part of this port.
template<typename T>
class ZPageTableParallelIterator {
public:
    explicit ZPageTableParallelIterator(const ZGranuleMap<T>& table);

    template<typename Function>
    void do_pages(Function function);

private:
    const ZGranuleMap<T>& table;
    ZIndexDistributorClaimTree distributor;
};

}

#include "Heap/z/zPageTable.inline.hpp"
