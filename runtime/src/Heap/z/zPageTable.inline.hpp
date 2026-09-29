// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zIndexDistributor.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include <limits>

namespace MapleRuntime {
inline int ZPageTable::count() const
{
    const size_t size = _map._size;
    assert(size <= static_cast<size_t>(std::numeric_limits<int>::max()));
    return static_cast<int>(size);
}

inline ZPageTableIterator::ZPageTableIterator(const ZPageTable* table)
    : _iter(&table->_map), _prev(nullptr)
{}

inline bool ZPageTableIterator::next(ZPage** page)
{
    for (ZPage* entry; _iter.next(&entry);) {
        if (entry != nullptr && entry != _prev) {
            *page = _prev = entry;
            return true;
        }
    }
    return false;
}

inline ZPageTableParallelIterator::ZPageTableParallelIterator(const ZPageTable* table)
    : _table(table), _index_distributor(table->count())
{}

template<typename Function>
inline void ZPageTableParallelIterator::do_pages(Function function)
{
    _index_distributor.do_indices([&](int index) {
        ZPage* const page = _table->at(static_cast<size_t>(index));
        if (page != nullptr) {
            const size_t start_index = untype(page->start()) >> ZGranuleSizeShift;
            if (static_cast<size_t>(index) == start_index) {
                return function(page);
            }
        }
        return true;
    });
}

} // namespace MapleRuntime
