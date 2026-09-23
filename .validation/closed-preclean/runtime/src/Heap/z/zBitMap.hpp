// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_BITMAP_HPP
#define MRT_Z_BITMAP_HPP

#include "Base/BitMap.h"

namespace MapleRuntime {

// ZGC zBitMap.hpp:29-33.
class ZMovableBitMap : public CHeapBitMap {
public:
    ZMovableBitMap();
    ZMovableBitMap(ZMovableBitMap&& bitmap);
};

// ZGC zBitMap.hpp:35-49. The pair (bit, bit + 1) is (live, strong).
class ZBitMap : public CHeapBitMap {
private:
    static bm_word_t bit_mask_pair(idx_t bit);

    bool par_set_bit_pair_finalizable(idx_t bit, bool& inc_live);
    bool par_set_bit_pair_strong(idx_t bit, bool& inc_live);

public:
    ZBitMap(idx_t size_in_bits);
    ZBitMap(const ZBitMap& other);


    bool par_set_bit_pair(idx_t bit, bool finalizable, bool& inc_live);

    class ReverseIterator;
};

// ZGC zBitMap.hpp:51-64.
class ZBitMap::ReverseIterator {
    BitMap* const _bitmap;
    BitMap::idx_t _beg;
    BitMap::idx_t _end;

public:
    ReverseIterator(BitMap* bitmap);
    ReverseIterator(BitMap* bitmap, BitMap::idx_t beg, BitMap::idx_t end);

    void reset(BitMap::idx_t beg, BitMap::idx_t end);
    void reset(BitMap::idx_t end);

    bool next(BitMap::idx_t* index);
};

} // namespace MapleRuntime

#endif // MRT_Z_BITMAP_HPP
