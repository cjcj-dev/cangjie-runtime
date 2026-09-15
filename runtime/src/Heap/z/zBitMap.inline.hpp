// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_BITMAP_INLINE_HPP
#define MRT_Z_BITMAP_INLINE_HPP

#include "Heap/z/zBitMap.hpp"

#include <cstring>

namespace MapleRuntime {

// ZGC zBitMap.inline.hpp:33-40.
inline ZMovableBitMap::ZMovableBitMap() : CHeapBitMap() {}

inline ZMovableBitMap::ZMovableBitMap(ZMovableBitMap&& bitmap) : CHeapBitMap()
{
    update(bitmap.map(), bitmap.size());
    bitmap.update(nullptr, 0);
}

// ZGC zBitMap.inline.hpp:42-49. ZBitMaps are not cleared when constructed.
inline ZBitMap::ZBitMap(idx_t size_in_bits) : CHeapBitMap(size_in_bits, false /* clear */) {}

inline ZBitMap::ZBitMap(const ZBitMap& other) : CHeapBitMap(other.size(), false /* clear */)
{
    std::memcpy(map(), other.map(), size_in_bytes());
}

// ZGC zBitMap.inline.hpp:51-54.
inline BitMap::bm_word_t ZBitMap::bit_mask_pair(idx_t bit)
{
    DCHECK_D(bit_in_word(bit) < BitsPerWord - 1, "Invalid bit index");
    return (bm_word_t)3 << bit_in_word(bit);
}

// ZGC zBitMap.inline.hpp:56-59. finalizable-live is the pair's first bit.
inline bool ZBitMap::par_set_bit_pair_finalizable(idx_t bit, bool& inc_live)
{
    inc_live = par_set_bit(bit);
    return inc_live;
}

// ZGC zBitMap.inline.hpp:61-83.
inline bool ZBitMap::par_set_bit_pair_strong(idx_t bit, bool& inc_live)
{
    verify_index(bit);
    volatile bm_word_t* const addr = word_addr(bit);
    const bm_word_t pair_mask = bit_mask_pair(bit);
    bm_word_t old_val = *addr;

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    if (testBeforeStrongCAS != nullptr) {
        testBeforeStrongCAS(this, bit);
    }
#endif

    do {
        const bm_word_t new_val = old_val | pair_mask;
        if (new_val == old_val) {
            // Someone else beat us to it
            inc_live = false;
            return false;
        }
        bm_word_t expected = old_val;
        if (__atomic_compare_exchange_n(addr, &expected, new_val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            // Success
            const bm_word_t marked_mask = bit_mask(bit);
            inc_live = !(old_val & marked_mask);
            return true;
        }

        // The value changed, retry
        old_val = expected;
    } while (true);
}

// ZGC zBitMap.inline.hpp:85-91. The strong/finalizable split is decided here.
inline bool ZBitMap::par_set_bit_pair(idx_t bit, bool finalizable, bool& inc_live)
{
    if (finalizable) {
        return par_set_bit_pair_finalizable(bit, inc_live);
    } else {
        return par_set_bit_pair_strong(bit, inc_live);
    }
}

// ZGC zBitMap.inline.hpp:93-122.
inline ZBitMap::ReverseIterator::ReverseIterator(BitMap* bitmap)
    : ZBitMap::ReverseIterator(bitmap, 0, bitmap->size()) {}

inline ZBitMap::ReverseIterator::ReverseIterator(BitMap* bitmap, BitMap::idx_t beg, BitMap::idx_t end)
    : _bitmap(bitmap),
      _beg(beg),
      _end(end) {}

inline void ZBitMap::ReverseIterator::reset(BitMap::idx_t beg, BitMap::idx_t end)
{
    DCHECK_D(beg < _bitmap->size(), "beg index out of bounds");
    DCHECK_D(end >= beg && end <= _bitmap->size(), "end index out of bounds");
    _beg = beg;
    _end = end;
}

inline void ZBitMap::ReverseIterator::reset(BitMap::idx_t end)
{
    DCHECK_D(end >= _beg && end <= _bitmap->size(), "end index out of bounds");
    _end = end;
}

inline bool ZBitMap::ReverseIterator::next(BitMap::idx_t* index)
{
    BitMap::ReverseIterator iter(*_bitmap, _beg, _end);
    if (iter.is_empty()) {
        return false;
    }

    *index = _end = iter.index();
    return true;
}

} // namespace MapleRuntime

#endif // MRT_Z_BITMAP_INLINE_HPP
