// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_REMEMBERED_SET_HPP
#define MRT_Z_REMEMBERED_SET_HPP

#include "Common/TypeDef.h"
#include "Heap/z/zBitMap.hpp"
#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {

class ZPage;

struct ZRememberedSetContaining {
    MAddress _field_addr;
    MAddress _addr;
};

class ZRememberedSetContainingIterator {
private:
    ZPage* const _page;
    ZBitMap::ReverseIterator _remset_iter;
    MAddress _obj;
    ZBitMap::ReverseIterator _obj_remset_iter;

    size_t to_index(MAddress addr);
    MAddress to_addr(BitMap::idx_t index);

public:
    explicit ZRememberedSetContainingIterator(ZPage* page);
    bool next(ZRememberedSetContaining* containing);
};

class ZRememberedSetContainingInLiveIterator {
private:
    ZRememberedSetContainingIterator _iter;
    MAddress _addr;
    size_t _addr_size;
    size_t _count;
    size_t _count_skipped;
    ZPage* const _page;

public:
    explicit ZRememberedSetContainingInLiveIterator(ZPage* page);
    bool next(ZRememberedSetContaining* containing);
    void print_statistics() const;
};

class ZRememberedSet {
    friend class ZRememberedSetContainingIterator;

public:
    static int _current;

    ZMovableBitMap _bitmap[2];

    CHeapBitMap* current();
    const CHeapBitMap* current() const;
    CHeapBitMap* previous();
    const CHeapBitMap* previous() const;

    template<typename Function>
    void iterate_bitmap(Function function, CHeapBitMap* bitmap);

    static uintptr_t to_offset(BitMap::idx_t index);
    static BitMap::idx_t to_index(uintptr_t offset);
    static BitMap::idx_t to_bit_size(size_t size);

    class Iterator {
        const CHeapBitMap* _bm;
        BitMap::idx_t _pos;
        BitMap::idx_t _end;

    public:
        Iterator(const CHeapBitMap& bm, BitMap::idx_t beg, BitMap::idx_t end)
            : _bm(&bm), _pos(beg), _end(end)
        {}
        bool next(BitMap::idx_t* index)
        {
            _pos = _bm->find_first_set_bit(_pos, _end);
            if (_pos >= _end) {
                return false;
            }
            *index = _pos;
            ++_pos;
            return true;
        }
    };

public:
    static void flip();

    ZRememberedSet();

    bool is_initialized() const;
    void initialize(size_t page_size);

    bool at_current(uintptr_t offset) const;
    bool at_previous(uintptr_t offset) const;
    bool set_current(uintptr_t offset);
    void unset_non_par_current(uintptr_t offset);
    void unset_range_non_par_current(uintptr_t offset, size_t size);

    template<typename Function>
    void iterate_previous(Function function);
    template<typename Function>
    void iterate_current(Function function);

    bool is_cleared_current() const;
    bool is_cleared_previous() const;

    void clear_previous();
    void swap_remset_bitmaps();

    ZBitMap::ReverseIterator iterator_reverse_previous();
    Iterator iterator_limited_current(uintptr_t offset, size_t size);
    Iterator iterator_limited_previous(uintptr_t offset, size_t size);
};

} // namespace MapleRuntime

#endif
