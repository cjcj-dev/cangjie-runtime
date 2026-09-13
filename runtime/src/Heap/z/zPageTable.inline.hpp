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
#pragma once
#include "Heap/z/zPageTable.hpp"

namespace MapleRuntime {
template<typename T>
inline ZPageTableParallelIterator<T>::ZPageTableParallelIterator(const ZGranuleMap<T>& table)
    : table(table), distributor(ZIndexDistributorClaimTree::get_count(table.size()))
{}
}

namespace MapleRuntime {
template<typename T>
template<typename Function>
inline void ZPageTableParallelIterator<T>::do_pages(Function function)
{
        distributor.do_indices([&](size_t index) {
            T page = table.at(index);
            if (page != T()) {
                const size_t startIndex = (page->GetRegionStart() - table.base()) / table.granule();
                if (index == startIndex) {
                    return function(page);
                }
            }
            return true;
        });
    }
}
