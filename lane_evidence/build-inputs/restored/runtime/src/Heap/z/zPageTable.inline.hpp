// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zIndexDistributor.inline.hpp"

namespace MapleRuntime {
template<typename T>
inline ZPageTableParallelIterator<T>::ZPageTableParallelIterator(const ZGranuleMap<T>& table)
    : table(table), distributor(static_cast<int>(ZIndexDistributor::get_count(table.size())))
{}

template<typename T>
template<typename Function>
inline void ZPageTableParallelIterator<T>::do_pages(Function function)
{
    distributor.do_indices([&](int index) {
        T page = table.at(static_cast<size_t>(index));
        if (page != T()) {
            const size_t startIndex = untype(page->start()) >> ZGranuleSizeShift;
            if (static_cast<size_t>(index) == startIndex) {
                return function(page);
            }
        }
        return true;
    });
}

} // namespace MapleRuntime
