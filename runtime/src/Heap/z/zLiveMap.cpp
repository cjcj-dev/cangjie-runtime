// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zLiveMap.hpp"

namespace MapleRuntime {
void RegionBitmap::EnsureSegmentLive(size_t pairBit)
    {
        const size_t segmentBits = SegmentBits();
        const size_t segment = pairBit / segmentBits;
        const uint64_t bit = uint64_t(1) << segment;
        if (IsSegmentLive(segment)) {
            return;
        }
        if ((segmentClaimBits.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
            while (!IsSegmentLive(segment)) {}
            return;
        }
        const size_t firstWord = segment * segmentBits / kBitsPerWord;
        const size_t endWord = std::min(firstWord + segmentBits / kBitsPerWord,
                                        wordCnt.load(std::memory_order_relaxed));
        for (size_t word = firstWord; word < endWord; ++word) {
            markWords[word].store(0, std::memory_order_relaxed);
        }
        segmentLiveBits.fetch_or(bit, std::memory_order_release);
    }
}

#include "Heap/z/zLiveMap.inline.hpp"
