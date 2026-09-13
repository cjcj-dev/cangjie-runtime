// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zLiveMap.hpp"

namespace MapleRuntime {
bool RegionBitmap::IsSegmentLive(size_t segment) const
    {
        return (segmentLiveBits.load(std::memory_order_acquire) & (uint64_t(1) << segment)) != 0;
    }
}

namespace MapleRuntime {
bool RegionBitmap::IsMarked(size_t start) const
    {
        const size_t pairBit = 2 * (start / kMarkedBytesPerBit);
        const size_t wordIdx = pairBit / kBitsPerWord;
        const uint64_t mask = static_cast<uint64_t>(2) << (pairBit % kBitsPerWord);
        return IsSegmentLive(pairBit / SegmentBits()) &&
            (markWords[wordIdx].load(std::memory_order_relaxed) & mask) != 0;
    }
}

namespace MapleRuntime {
void RegionBitmap::AddLiveCounts(size_t objects, size_t bytes)
    {
        liveObjects.fetch_add(objects, std::memory_order_relaxed);
        liveBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

namespace MapleRuntime {
size_t RegionBitmap::GetLiveObjects() const { return liveObjects.load(std::memory_order_relaxed); }
}

namespace MapleRuntime {
bool RegionBitmap::MarkBits(size_t start, size_t byteCnt, size_t regionSize, bool& incLive)
    {
        (void)regionSize;
        (void)byteCnt;
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, maskInfo);
        EnsureSegmentLive(2 * (start / kMarkedBytesPerBit));
        // ZGC zBitMap.inline.hpp:60-83 / zLiveMap: only the object-start pair.
        // find_base_bit finds last set bit then aligns to the pair (zLiveMap.inline.hpp:219-221).
        const uint64_t startPair = maskInfo.liveStartBitMask | maskInfo.strongStartBitMask;
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(startPair);
        const bool already = (old & maskInfo.strongStartBitMask) != 0;
        incLive = !already && (old & maskInfo.liveStartBitMask) == 0;
        return already;
    }
}

namespace MapleRuntime {
bool RegionBitmap::MarkBits(size_t start, size_t byteCnt, size_t regionSize)
    {
        bool incLive = false;
        const bool already = MarkBits(start, byteCnt, regionSize, incLive);
        if (incLive) {
            AddLiveCounts(1, byteCnt);
        }
        return already;
    }
}

namespace MapleRuntime {
bool RegionBitmap::MarkFinalizableBits(size_t start, size_t byteCnt, size_t regionSize, bool& incLive)
    {
        (void)regionSize;
        (void)byteCnt;
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, maskInfo);
        EnsureSegmentLive(2 * (start / kMarkedBytesPerBit));
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(maskInfo.liveStartBitMask);
        const bool already = (old & maskInfo.liveStartBitMask) != 0;
        incLive = !already;
        return already;
    }
}

namespace MapleRuntime {
bool RegionBitmap::IsLive(size_t start) const
    {
        const size_t pairBit = 2 * (start / kMarkedBytesPerBit);
        const size_t wordIdx = pairBit / kBitsPerWord;
        const uint64_t mask = static_cast<uint64_t>(1) << (pairBit % kBitsPerWord);
        return IsSegmentLive(pairBit / SegmentBits()) &&
            (markWords[wordIdx].load(std::memory_order_relaxed) & mask) != 0;
    }
}

namespace MapleRuntime {
bool RegionBitmap::IsFinalizable(size_t start) const { return IsLive(start) && !IsMarked(start); }
}

namespace MapleRuntime {
size_t RegionBitmap::GetLiveBytes() const { return liveBytes.load(std::memory_order_acquire); }
}

namespace MapleRuntime {
void RegionBitmap::Reset()
    {
        liveBytes.store(0, std::memory_order_relaxed);
        liveObjects.store(0, std::memory_order_relaxed);
        segmentLiveBits.store(0, std::memory_order_relaxed);
        segmentClaimBits.store(0, std::memory_order_relaxed);
    }
}
