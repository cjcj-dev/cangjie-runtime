// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_LIVEMAP_INLINE_HPP
#define MRT_Z_LIVEMAP_INLINE_HPP

#include "Heap/z/zLiveMap.hpp"
#include "Heap/z/zGeneration.hpp"

#include "Base/Log.h"
#include "Heap/z/zBitMap.inline.hpp"

namespace MapleRuntime {

// ZGC zLiveMap.inline.hpp:37-39.
inline void ZLiveMap::reset()
{
    _seqnum.store(0u, std::memory_order_relaxed);
}

// ZGC zLiveMap.inline.hpp:41-43: the generation seqnum is read at call time.
inline bool ZLiveMap::is_marked(ZGenerationId id) const
{
    return _seqnum.load(std::memory_order_acquire) == ZGeneration::generation(id)->seqnum();
}

inline uint32_t ZLiveMap::live_objects() const
{
    return _live_objects.load(std::memory_order_relaxed);
}

inline size_t ZLiveMap::live_bytes() const
{
    return _live_bytes.load(std::memory_order_relaxed);
}

inline const BitMapView ZLiveMap::segment_live_bits() const
{
    return BitMapView(const_cast<BitMap::bm_word_t*>(&_segment_live_bits), NumSegments);
}

inline const BitMapView ZLiveMap::segment_claim_bits() const
{
    return BitMapView(const_cast<BitMap::bm_word_t*>(&_segment_claim_bits), NumSegments);
}

inline BitMapView ZLiveMap::segment_live_bits()
{
    return BitMapView(&_segment_live_bits, NumSegments);
}

inline BitMapView ZLiveMap::segment_claim_bits()
{
    return BitMapView(&_segment_claim_bits, NumSegments);
}

inline bool ZLiveMap::is_segment_live(BitMap::idx_t segment) const
{
    return segment_live_bits().par_at(segment);
}

inline bool ZLiveMap::set_segment_live(BitMap::idx_t segment)
{
    return segment_live_bits().par_set_bit(segment, std::memory_order_release);
}

inline bool ZLiveMap::claim_segment(BitMap::idx_t segment)
{
    return segment_claim_bits().par_set_bit(segment, std::memory_order_acq_rel);
}

inline BitMap::idx_t ZLiveMap::first_live_segment() const
{
    return segment_live_bits().find_first_set_bit(0, NumSegments);
}

inline BitMap::idx_t ZLiveMap::next_live_segment(BitMap::idx_t segment) const
{
    return segment_live_bits().find_first_set_bit(segment + 1, NumSegments);
}

inline BitMap::idx_t ZLiveMap::index_to_segment(BitMap::idx_t index) const
{
    return index >> _segment_shift;
}

// ZGC zLiveMap.inline.hpp:93-98.
inline bool ZLiveMap::get(ZGenerationId id, BitMap::idx_t index) const
{
    const BitMap::idx_t segment = index_to_segment(index);
    return is_marked(id) &&                                    // Page is marked
           is_segment_live(segment) &&                         // Segment is marked
           _bitmap.par_at(index, std::memory_order_relaxed);   // Object is marked
}

// ZGC zLiveMap.inline.hpp:100-115.
inline bool ZLiveMap::set(ZGenerationId id, BitMap::idx_t index, bool finalizable, bool& inc_live)
{
    if (!is_marked(id)) {
        // First object to be marked during this
        // cycle, reset marking information.
        reset(id);
    }

    const BitMap::idx_t segment = index_to_segment(index);
    if (!is_segment_live(segment)) {
        // First object to be marked in this segment during
        // this cycle, reset segment bitmap.
        reset_segment(segment);
    }

    return _bitmap.par_set_bit_pair(index, finalizable, inc_live);
}

// ZGC zLiveMap.inline.hpp:117-120.
inline void ZLiveMap::inc_live(uint32_t objects, size_t bytes)
{
    _live_objects.fetch_add(objects, std::memory_order_relaxed);
    _live_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

inline BitMap::idx_t ZLiveMap::segment_start(BitMap::idx_t segment) const
{
    return segment * _segment_size;
}

inline BitMap::idx_t ZLiveMap::segment_end(BitMap::idx_t segment) const
{
    return segment_start(segment) + _segment_size;
}

// ZGC zLiveMap.inline.hpp:130-139.
template <typename Function>
inline void ZLiveMap::iterate_segment(BitMap::idx_t segment, Function function)
{
    DCHECK_D(is_segment_live(segment), "Must be");

    const BitMap::idx_t start_index = segment_start(segment);
    const BitMap::idx_t end_index = segment_end(segment);

    _bitmap.iterate(function, start_index, end_index);
}

// ZGC zLiveMap.inline.hpp:141-158: skip segments never marked this cycle and
// only visit the first bit of each pair.
template <typename Function>
inline void ZLiveMap::iterate(ZGenerationId id, Function function)
{
    if (!is_marked(id)) {
        return;
    }

    auto live_only = [&](BitMap::idx_t index) -> bool {
        if ((index & 1) == 0) {
            return function(index);
        }
        // Don't visit the finalizable bits
        return true;
    };

    for (BitMap::idx_t segment = first_live_segment(); segment < NumSegments; segment = next_live_segment(segment)) {
        // For each live segment
        iterate_segment(segment, live_only);
    }
}

// ZGC zLiveMap.inline.hpp:160-222.
// Find the bit index that correspond the start of the object that is lower,
// or equal, to the given index (index is inclusive).
//
// Typically used to find the start of an object when there's only a field
// address available. Note that it's not guaranteed that the found index
// corresponds to an object that spans the given index. This function just
// looks at the bits. The calling code is responsible to check the object
// at the returned index.
//
// returns -1 if no bit was found
inline BitMap::idx_t ZLiveMap::find_base_bit(BitMap::idx_t index)
{
    // Check first segment
    const BitMap::idx_t start_segment = index_to_segment(index);
    if (is_segment_live(start_segment)) {
        const BitMap::idx_t res = find_base_bit_in_segment(segment_start(start_segment), index);
        if (res != BitMap::idx_t(-1)) {
            return res;
        }
    }

    // Search earlier segments
    for (BitMap::idx_t segment = start_segment; segment-- > 0;) {
        if (is_segment_live(segment)) {
            const BitMap::idx_t res = find_base_bit_in_segment(segment_start(segment), segment_end(segment) - 1);
            if (res != BitMap::idx_t(-1)) {
                return res;
            }
        }
    }

    // Not found
    return BitMap::idx_t(-1);
}

// Find the bit index that correspond the start of the object that is lower,
// or equal, to the given index (index is inclusive). Stopping when reaching
// start.
inline BitMap::idx_t ZLiveMap::find_base_bit_in_segment(BitMap::idx_t start, BitMap::idx_t index)
{
    DCHECK_D(index_to_segment(start) == index_to_segment(index), "Only supports searches within segments");
    DCHECK_D(is_segment_live(index_to_segment(start)), "Must be live");

    // Search backwards - + 1 to make an exclusive index.
    const BitMap::idx_t end = index + 1;
    const BitMap::idx_t bit = _bitmap.find_last_set_bit(start, end);
    if (bit == end) {
        return BitMap::idx_t(-1);
    }

    // The bitmaps contain pairs of bits to deal with strongly marked vs only
    // finalizable marked. Align down to get the first bit position.
    return bit & ~BitMap::idx_t(1);
}

} // namespace MapleRuntime

#endif // MRT_Z_LIVEMAP_INLINE_HPP
