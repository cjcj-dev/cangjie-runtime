// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_REMEMBERED_SET_INLINE_HPP
#define MRT_Z_REMEMBERED_SET_INLINE_HPP

#include "Heap/z/zRememberedSet.hpp"
#include "ObjectModel/RefField.h"
#include "Heap/z/zBitMap.inline.hpp"

namespace MapleRuntime {

inline CHeapBitMap* ZRememberedSet::current()
{
    return &_bitmap[_current];
}

inline const CHeapBitMap* ZRememberedSet::current() const
{
    return &_bitmap[_current];
}

inline CHeapBitMap* ZRememberedSet::previous()
{
    return &_bitmap[_current ^ 1];
}

inline const CHeapBitMap* ZRememberedSet::previous() const
{
    return &_bitmap[_current ^ 1];
}

inline uintptr_t ZRememberedSet::to_offset(BitMap::idx_t index)
{
    return index * sizeof(RefField<>);
}

inline BitMap::idx_t ZRememberedSet::to_index(uintptr_t offset)
{
    return offset / sizeof(RefField<>);
}

inline BitMap::idx_t ZRememberedSet::to_bit_size(size_t size)
{
    return size / sizeof(RefField<>);
}

inline bool ZRememberedSet::at_current(uintptr_t offset) const
{
    return current()->at(to_index(offset));
}

inline bool ZRememberedSet::at_previous(uintptr_t offset) const
{
    return previous()->at(to_index(offset));
}

inline bool ZRememberedSet::set_current(uintptr_t offset)
{
    return current()->par_set_bit(to_index(offset), std::memory_order_relaxed);
}

inline void ZRememberedSet::unset_non_par_current(uintptr_t offset)
{
    current()->clear_bit(to_index(offset));
}

inline void ZRememberedSet::unset_range_non_par_current(uintptr_t offset, size_t size)
{
    current()->clear_range(to_index(offset), to_index(offset + size));
}

template<typename Function>
void ZRememberedSet::iterate_bitmap(Function function, CHeapBitMap* bitmap)
{
    bitmap->iterate([&](BitMap::idx_t index) {
        function(to_offset(index));
        return true;
    });
}

template<typename Function>
void ZRememberedSet::iterate_previous(Function function)
{
    iterate_bitmap(function, previous());
}

template<typename Function>
void ZRememberedSet::iterate_current(Function function)
{
    iterate_bitmap(function, current());
}

} // namespace MapleRuntime

#endif
