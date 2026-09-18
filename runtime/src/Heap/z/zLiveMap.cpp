// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zLiveMap.inline.hpp"

#include <sched.h>

#include "Base/Log.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zStat.hpp"

namespace MapleRuntime {

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
// P02's three scheduling points share this product TU. They are absent from
// the default build; tests install them before spawning and clear after join.
void (*BitMap::testAfterPartialClearLoad)(const volatile bm_word_t*) = nullptr;
void (*ZBitMap::testBeforeStrongCAS)(const ZBitMap*, idx_t) = nullptr;
void (*ZLiveMap::testReset)(const ZLiveMap*, bool) = nullptr;
#endif

// ZGC zLiveMap.cpp:34-35.
static const ZStatCounter ZCounterMarkSeqNumResetContention("Contention", "Mark SeqNum Reset Contention",
                                                            ZStatUnitOpsPerSecond);
static const ZStatCounter ZCounterMarkSegmentResetContention("Contention", "Mark Segment Reset Contention",
                                                             ZStatUnitOpsPerSecond);

// ZGeneration::generation(id)->seqnum(): the per-generation cycle sequence.
uint64_t ZLiveMap::generation_seqnum(ZGenerationId id)
{
    const ZGenerationId generation = id == ZGenerationId::young ? ZGenerationId::young
                                                                    : ZGenerationId::old;
    return Heap::GetHeap().GetZGeneration(generation).Snapshot().sequence;
}

// ZGC zLiveMap.cpp:38: (object_max_count / NumSegments) * BitsPerObject. Cangjie
// pages are unit multiples rather than power-of-two page sizes, so the
// segment width is rounded up to the next power of two to keep the shift
// (index_to_segment) exact; the 64 segments then cover at least
// object_max_count pairs. Unlike ZGC's 2MB small pages, a Cangjie small page
// can be 4KB. Independent segments must still own whole bitmap words:
// reset_segment clears without an atomic RMW (ZGC zLiveMap.cpp:129-136).
// The single-object large page has no competing segment and keeps two bits.
uint32_t ZLiveMap::segment_size(uint32_t object_max_count)
{
    const uint32_t objects_per_segment =
        object_max_count == 1 ? 1u : (object_max_count + NumSegments - 1) / NumSegments;
    uint32_t size = object_max_count == 1 ? BitsPerObject : BitMap::BitsPerWord;
    while (size < objects_per_segment * BitsPerObject) {
        size <<= 1;
    }
    return size;
}

int ZLiveMap::segment_shift(uint32_t segment_size)
{
    CHECK_DETAIL(segment_size != 0 && (segment_size & (segment_size - 1)) == 0,
                 "livemap segment size must be a power of two: %u", segment_size);
    int shift = 0;
    while ((1u << shift) != segment_size) {
        ++shift;
    }
    return shift;
}

// ZGC zLiveMap.cpp:37-45.
ZLiveMap::ZLiveMap(uint32_t object_max_count)
    : _segment_size(segment_size(object_max_count)),
      _segment_shift(segment_shift(_segment_size)),
      _seqnum(0),
      _live_objects(0),
      _live_bytes(0),
      _segment_live_bits(0),
      _segment_claim_bits(0),
      _bitmap(0) {}

// ZGC zLiveMap.cpp:47-51: the bitmap storage is allocated on the first mark.
void ZLiveMap::initialize_bitmap()
{
    if (_bitmap.size() == 0) {
        _bitmap.initialize(size_t(_segment_size) * size_t(NumSegments), false /* clear */);
    }
}

// ZGC zLiveMap.cpp:53-107.
void ZLiveMap::reset(ZGenerationId id)
{
    const uint64_t generation_seqnum_now = generation_seqnum(id);
    const uint64_t seqnum_initializing = (uint64_t)-1;
    bool contention = false;

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
    if (testReset != nullptr) {
        testReset(this, false);
    }
#endif

    // Multiple threads can enter here, make sure only one of them
    // resets the marking information while the others busy wait.
    for (uint64_t seqnum = _seqnum.load(std::memory_order_acquire);
         seqnum != generation_seqnum_now;
         seqnum = _seqnum.load(std::memory_order_acquire)) {

        if (seqnum != seqnum_initializing) {
            // No one has claimed initialization of the livemap yet
            if (_seqnum.compare_exchange_strong(seqnum, seqnum_initializing, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                // This thread claimed the initialization

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
                if (testReset != nullptr) {
                    testReset(this, true);
                }
#endif

                // Reset marking information
                _live_bytes.store(0u, std::memory_order_relaxed);
                _live_objects.store(0u, std::memory_order_relaxed);

                // Clear segment claimed/live bits
                segment_live_bits().clear();
                segment_claim_bits().clear();

                // We lazily initialize the bitmap the first time the page is marked, i.e.
                // a bit is about to be set for the first time.
                initialize_bitmap();

                DCHECK_D(_seqnum.load(std::memory_order_relaxed) == seqnum_initializing, "Invalid");

                // Make sure the newly reset marking information is ordered
                // before the update of the page seqnum, such that when the
                // up-to-date seqnum is load acquired, the bit maps will not
                // contain stale information.
                _seqnum.store(generation_seqnum_now, std::memory_order_release);
                break;
            }
        }

        // Mark reset contention
        if (!contention) {
            // Count contention once
            ZStatInc(ZCounterMarkSeqNumResetContention, 1);
            contention = true;

            DLOG(TRACE, "Mark seqnum reset contention, map: %p", static_cast<void*>(this));
        }

        // "Yield" to allow the thread that's resetting the livemap to finish
        sched_yield();
    }
}

// ZGC zLiveMap.cpp:109-142.
void ZLiveMap::reset_segment(BitMap::idx_t segment)
{
    bool contention = false;

    if (!claim_segment(segment)) {
        // Already claimed, wait for live bit to be set
        while (!is_segment_live(segment)) {
            // Mark reset contention
            if (!contention) {
                // Count contention once
                ZStatInc(ZCounterMarkSegmentResetContention, 1);
                contention = true;

                DLOG(TRACE, "Mark segment reset contention, map: %p, segment: %zu", static_cast<void*>(this),
                     segment);
            }
        }

        // Segment is live
        return;
    }

    // Segment claimed, clear it
    const BitMap::idx_t start_index = segment_start(segment);
    const BitMap::idx_t end_index = segment_end(segment);
    if (_segment_size / BitMap::BitsPerWord >= 32) {
        _bitmap.clear_large_range(start_index, end_index);
    } else {
        _bitmap.clear_range(start_index, end_index);
    }

    // Set live bit
    const bool success = set_segment_live(segment);
    DCHECK_D(success, "Should never fail");
    (void)success;
}

} // namespace MapleRuntime
