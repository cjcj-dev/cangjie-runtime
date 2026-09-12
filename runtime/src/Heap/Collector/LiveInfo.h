// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LIVE_INFO_H
#define MRT_LIVE_INFO_H
#include <cstdint>
#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/SysCall.h"
#include "Heap/Heap.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace MapleRuntime {
using RegionLifeId = uint64_t;
constexpr size_t kBitsPerByte = 8;
constexpr size_t kMarkedBytesPerBit = 8;
constexpr size_t kBitsPerWord = sizeof(uint64_t) * kBitsPerByte;
class RegionInfo;

// The collector whose transitive closure owns a mark face.  This is deliberately
// distinct from RegionInfo::_generation_id: major marking visits the whole heap,
// including regions which are currently young.
enum class Generation : uint8_t {
    Young = 0,
    Old = 1,
};

// A mark read is not representable without naming the closure which produced it.
// Construction is restricted to RegionInfo so MarkView<Young> can reject an old
// region at the minting boundary.  The two instantiations intentionally have no
// conversion between them; runtime/tests/mark_generation_compile_probe.cpp keeps
// that property under an always-run negative compile gate.
template<Generation G>
class MarkView {
public:
    RegionInfo* GetRegion() const { return region; }
    uint64_t GetEpoch() const { return epoch; }
    RegionLifeId GetLifeId() const { return lifeId; }

private:
    MarkView(RegionInfo* regionIn, uint64_t epochIn, RegionLifeId lifeIdIn)
        : region(regionIn), epoch(epochIn), lifeId(lifeIdIn)
    {
    }

    RegionInfo* region;
    uint64_t epoch;
    RegionLifeId lifeId;

    friend class RegionInfo;
};

struct RegionBitmap {
    // A 64-bit mark word carries one live/finalizable + strong pair per slot.
    // Keep this geometry in one named constant so allocation and test fixtures
    // cannot silently drift back to the pre-pair 512-byte rule.
    static constexpr size_t kRegionBytesPerWord =
        (kMarkedBytesPerBit * kBitsPerWord) / 2;
    std::atomic<size_t> liveBytes;
    // Two adjacent bits describe each 8-byte slot: live/finalizable then
    // strong. One word therefore covers 32 slots (256 region bytes).
    std::atomic<size_t> wordCnt;
    std::atomic<uint64_t> markWords[0];

    static size_t GetRegionBitmapSize(size_t regionSize)
    {
        const size_t words = regionSize / kRegionBytesPerWord;
        return sizeof(RegionBitmap) + (words * sizeof(uint64_t));
    }

    struct BitMaskInfo {
        size_t headWordIdx;
        uint64_t liveStartBitMask;
        uint64_t strongStartBitMask;
    };

    static void GetBitMaskInfo(size_t start, BitMaskInfo& maskInfo)
    {
        const size_t pairBitStart = 2 * (start / kMarkedBytesPerBit);
        size_t headMaskBitStart = pairBitStart % kBitsPerWord;
        maskInfo.headWordIdx = pairBitStart / kBitsPerWord;
        maskInfo.liveStartBitMask = static_cast<uint64_t>(1) << headMaskBitStart;
        maskInfo.strongStartBitMask = static_cast<uint64_t>(1) << (headMaskBitStart + 1);
    }

    void AddLiveBytes(size_t byteCnt)
    {
        liveBytes.fetch_add(byteCnt);
    }

    explicit RegionBitmap(size_t regionSize)
        : liveBytes(0), wordCnt(regionSize / kRegionBytesPerWord)
    {}

    bool CoversRegionSize(size_t regionSize) const
    {
        return wordCnt.load(std::memory_order_relaxed) == regionSize / kRegionBytesPerWord;
    }

    // Reset the bitmap state without exposing markWords/wordCnt to tests.
    // Keeping this operation on the carrier makes the concurrent invariant
    // independent of the number of words or any future pair packing.
    void Reset()
    {
        liveBytes.store(0, std::memory_order_relaxed);
        const size_t words = wordCnt.load(std::memory_order_relaxed);
        for (size_t idx = 0; idx < words; ++idx) {
            markWords[idx].store(0, std::memory_order_relaxed);
        }
    }

    bool MarkBits(size_t start, size_t byteCnt, size_t regionSize, bool& incLive)
    {
        (void)regionSize;
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, maskInfo);
        // ZGC zBitMap.inline.hpp:60-83 / zLiveMap: only the object-start pair.
        // find_base_bit finds last set bit then aligns to the pair (zLiveMap.inline.hpp:219-221).
        const uint64_t startPair = maskInfo.liveStartBitMask | maskInfo.strongStartBitMask;
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(startPair);
        const bool already = (old & maskInfo.strongStartBitMask) != 0;
        incLive = !already && (old & maskInfo.liveStartBitMask) == 0;
        if (incLive) {
            AddLiveBytes(byteCnt);
        }
        return already;
    }

    bool MarkBits(size_t start, size_t byteCnt, size_t regionSize)
    {
        bool incLive = false;
        return MarkBits(start, byteCnt, regionSize, incLive);
    }

    bool MarkFinalizableBits(size_t start, size_t byteCnt, size_t regionSize, bool& incLive)
    {
        (void)regionSize;
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, maskInfo);
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(maskInfo.liveStartBitMask);
        const bool already = (old & maskInfo.liveStartBitMask) != 0;
        incLive = !already;
        if (incLive) {
            AddLiveBytes(byteCnt);
        }
        return already;
    }

    bool IsMarked(size_t start) const
    {
        const size_t pairBit = 2 * (start / kMarkedBytesPerBit);
        const size_t wordIdx = pairBit / kBitsPerWord;
        const uint64_t mask = static_cast<uint64_t>(2) << (pairBit % kBitsPerWord);
        return (markWords[wordIdx].load(std::memory_order_acquire) & mask) != 0;
    }

    bool IsLive(size_t start) const
    {
        const size_t pairBit = 2 * (start / kMarkedBytesPerBit);
        const size_t wordIdx = pairBit / kBitsPerWord;
        const uint64_t mask = static_cast<uint64_t>(1) << (pairBit % kBitsPerWord);
        return (markWords[wordIdx].load(std::memory_order_acquire) & mask) != 0;
    }

    bool IsFinalizable(size_t start) const { return IsLive(start) && !IsMarked(start); }

    // zLiveMap.inline.hpp:219-221: pair with either strong or finalizable bit is an object start.
    bool IsObjectStart(size_t start) const { return IsLive(start) || IsMarked(start); }

    size_t GetLiveBytes() const { return liveBytes.load(std::memory_order_acquire); }

    size_t RecomputeLiveBytes() const { return GetLiveBytes(); }
};
struct LiveInfo {
    static constexpr MAddress TEMPORARY_PTR = 0x1234;
    RegionInfo* bindedRegion = nullptr;
    RegionBitmap* resurrectBitmap = nullptr;
    RegionBitmap* enqueueBitmap = nullptr;

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, size_t offset) const
    {
        const MarkFace& face = GetMarkFace();
        RegionBitmap* markBitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        if (face.epoch.load(std::memory_order_acquire) == view.GetEpoch() && markBitmap != nullptr &&
            reinterpret_cast<MAddress>(markBitmap) != TEMPORARY_PTR && markBitmap->IsLive(offset)) {
            return true;
        }
        // Resurrection is a major/old decision.  A young closure is not complete
        // for old/large objects and must not inherit an old resurrection verdict.
        return G == Generation::Old && resurrectBitmap != nullptr &&
            reinterpret_cast<MAddress>(resurrectBitmap) != TEMPORARY_PTR && resurrectBitmap->IsMarked(offset);
    }

    template<Generation G>
    size_t GetBitmapLiveBytes(MarkView<G> view) const
    {
        const MarkFace& face = GetMarkFace();
        RegionBitmap* markBitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        const bool current = face.epoch.load(std::memory_order_acquire) == view.GetEpoch();
        return (!current || markBitmap == nullptr ? 0 : markBitmap->GetLiveBytes()) +
            (G != Generation::Old || resurrectBitmap == nullptr ? 0 : resurrectBitmap->GetLiveBytes());
    }

    template<Generation G>
    size_t RecomputeBitmapLiveBytes(MarkView<G> view) const
    {
        const MarkFace& face = GetMarkFace();
        RegionBitmap* markBitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        const bool current = face.epoch.load(std::memory_order_acquire) == view.GetEpoch();
        return (!current || markBitmap == nullptr ? 0 : markBitmap->RecomputeLiveBytes()) +
            (G != Generation::Old || resurrectBitmap == nullptr ? 0 : resurrectBitmap->RecomputeLiveBytes());
    }

private:
    struct MarkFace {
        // ZGC ZLiveMap::_seqnum counterpart, one per page metadata incarnation.
        std::atomic<uint64_t> epoch{ 0 };
        RegionBitmap* bitmap = nullptr;
    };

    // A page metadata object owns exactly one ordinary livemap. Generation is
    // carried by the page/current-or-from metadata which owns this LiveInfo,
    // never by a second bitmap hidden inside the same carrier.
    MarkFace markFace;

    MarkFace& GetMarkFace()
    {
        return markFace;
    }

    const MarkFace& GetMarkFace() const
    {
        return markFace;
    }

    friend class RegionInfo;
};

} // namespace MapleRuntime
#endif // MRT_LIVE_INFO_H
