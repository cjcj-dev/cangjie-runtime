// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_LIVEMAP_HPP
#define MRT_Z_LIVEMAP_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Base/BitMap.h"
#include "Heap/z/zBitMap.hpp"
#include "Heap/z/zGenerationId.hpp"

namespace MapleRuntime {

// ZGC zLiveMap.hpp:35-101. One object per page: the marking seqnum, the live
// counters, the 64 segment live/claim bits and the lazily initialized bit-pair
// map live together and are reset together (reset(id)).
class ZLiveMap {
    friend class ZLiveMapTest;

private:
    static const uint32_t NumSegments = 64;
    static const uint32_t BitsPerObject = 2;

    const uint32_t _segment_size;
    const int _segment_shift;

    std::atomic<uint64_t> _seqnum;
    std::atomic<uint32_t> _live_objects;
    std::atomic<size_t> _live_bytes;
    BitMap::bm_word_t _segment_live_bits;
    BitMap::bm_word_t _segment_claim_bits;
    ZBitMap _bitmap;

    static uint32_t segment_size(uint32_t object_max_count);
    static int segment_shift(uint32_t segment_size);

    const BitMapView segment_live_bits() const;
    const BitMapView segment_claim_bits() const;

    BitMapView segment_live_bits();
    BitMapView segment_claim_bits();

    BitMap::idx_t segment_start(BitMap::idx_t segment) const;
    BitMap::idx_t segment_end(BitMap::idx_t segment) const;

    bool is_segment_live(BitMap::idx_t segment) const;
    bool set_segment_live(BitMap::idx_t segment);

    BitMap::idx_t first_live_segment() const;
    BitMap::idx_t next_live_segment(BitMap::idx_t segment) const;
    BitMap::idx_t index_to_segment(BitMap::idx_t index) const;

    bool claim_segment(BitMap::idx_t segment);

    void initialize_bitmap();

    void reset(ZGenerationId id);
    void reset_segment(BitMap::idx_t segment);

    template <typename Function>
    void iterate_segment(BitMap::idx_t segment, Function function);

public:
#if defined(MRT_TESTABLE_INTERNALS)
    // P02: observe entry/claim to order real resetters, without changing state.
    MRT_EXPORT static void (*testReset)(const ZLiveMap*, bool claimed);
#endif

    // ZGeneration::generation(id)->seqnum() (zGeneration.inline.hpp). The
    // generation object is owned by the generation package; the sequence is
    // read live from the collector's per-generation cycle state.
    static uint64_t generation_seqnum(ZGenerationId id);

    ZLiveMap(uint32_t object_max_count);
    ZLiveMap(const ZLiveMap& other) = delete;

    void reset();

    bool is_marked(ZGenerationId id) const;

    uint32_t live_objects() const;
    size_t live_bytes() const;

    bool get(ZGenerationId id, BitMap::idx_t index) const;
    bool set(ZGenerationId id, BitMap::idx_t index, bool finalizable, bool& inc_live);

    void inc_live(uint32_t objects, size_t bytes);

    template <typename Function>
    void iterate(ZGenerationId id, Function function);

    BitMap::idx_t find_base_bit(BitMap::idx_t index);
    BitMap::idx_t find_base_bit_in_segment(BitMap::idx_t start, BitMap::idx_t index);
};

} // namespace MapleRuntime

#endif // MRT_Z_LIVEMAP_HPP
