// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zIndexDistributor.inline.hpp"
#include <limits>

namespace MapleRuntime {
inline int ZPageTable::count() const
{
    const size_t size = _map._size;
    assert(size <= static_cast<size_t>(std::numeric_limits<int>::max()));
    return static_cast<int>(size);
}

inline ZPageTableParallelIterator::ZPageTableParallelIterator(const ZPageTable* table)
    : _table(table), _index_distributor(table->count())
{}


} // namespace MapleRuntime
