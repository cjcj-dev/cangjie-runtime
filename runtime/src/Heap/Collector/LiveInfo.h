// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LIVE_INFO_H
#define MRT_LIVE_INFO_H
#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/SysCall.h"
#include "Heap/Heap.h"
#include "Heap/Collector/RegionLifeClock.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace MapleRuntime {
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
    static constexpr uint8_t factor = 16;
    // A 64-bit mark word carries one live/finalizable + strong pair per slot.
    // Keep this geometry in one named constant so allocation and test fixtures
    // cannot silently drift back to the pre-pair 512-byte rule.
    static constexpr size_t kRegionBytesPerWord =
        (kMarkedBytesPerBit * kBitsPerWord) / 2;
    std::atomic<uint16_t> partLiveBytes[factor];
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
        uint64_t headMaskBits;
        // Single bit for the object start (offset/8). "Already marked" must mean this bit,
        // not any bit in the multi-byte head mask — otherwise a neighbor mark makes MarkBits
        // return already without setting the start bit, and CHECK(IsMarkedObject) ABRTs.
        uint64_t liveStartBitMask;
        uint64_t strongStartBitMask;
        size_t tailWordCnt; // count of mask words excludes the start mask
        uint64_t lastMaskBits;
    };

    static void GetBitMaskInfo(size_t start, size_t byteCnt, BitMaskInfo& maskInfo)
    {
        const size_t pairBitStart = 2 * (start / kMarkedBytesPerBit);
        size_t headWordIdx = pairBitStart / kBitsPerWord;
        size_t headMaskBitStart = pairBitStart % kBitsPerWord;
        maskInfo.headWordIdx = headWordIdx;
        maskInfo.liveStartBitMask = static_cast<uint64_t>(1) << headMaskBitStart;
        maskInfo.strongStartBitMask = static_cast<uint64_t>(1) << (headMaskBitStart + 1);

        size_t headBitCnt = kBitsPerWord - headMaskBitStart;
        size_t maskBitCnt = 2 * (byteCnt / kMarkedBytesPerBit);
        if (maskBitCnt >= headBitCnt) {
            size_t tailBitCnt = maskBitCnt - headBitCnt;
            size_t tailWordCnt = (tailBitCnt + kBitsPerWord - 1) / kBitsPerWord;
            size_t lastBitCnt = tailBitCnt % kBitsPerWord;
            uint64_t lastMaskBits = (static_cast<uint64_t>(1) << lastBitCnt) - 1;
            maskInfo.headMaskBits = ~((static_cast<uint64_t>(1) << headMaskBitStart) - 1);
            maskInfo.tailWordCnt = tailWordCnt;
            maskInfo.lastMaskBits = lastMaskBits;
        } else {
            size_t headMaskBitEnd = headMaskBitStart + maskBitCnt;
            uint64_t headMaskBits = (static_cast<uint64_t>(1) << headMaskBitEnd) - 1;
            maskInfo.headMaskBits = (headMaskBits >> headMaskBitStart) << headMaskBitStart;
            maskInfo.tailWordCnt = 0;
            maskInfo.lastMaskBits = 0;
        }
    }

    static constexpr uint64_t kLiveBitMask = 0x5555555555555555ULL;

    void SetTailMask(const BitMaskInfo& maskInfo, uint64_t planeMask)
    {
        if (maskInfo.tailWordCnt == 0) {
            return;
        }
        const size_t lastWordIdx = maskInfo.headWordIdx + maskInfo.tailWordCnt;
        const size_t fullEnd = maskInfo.lastMaskBits == 0 ? lastWordIdx + 1 : lastWordIdx;
        for (size_t idx = maskInfo.headWordIdx + 1; idx < fullEnd; ++idx) {
            markWords[idx].fetch_or(planeMask);
        }
        if (maskInfo.lastMaskBits != 0) {
            markWords[lastWordIdx].fetch_or(maskInfo.lastMaskBits & planeMask);
        }
    }

    void AddLiveBytesForMask(const BitMaskInfo& maskInfo, size_t byteCnt, size_t regionSize)
    {
        size_t markWordSize = regionSize / kRegionBytesPerWord;
        uint8_t calFactor = factor > markWordSize ? markWordSize : factor;
        if (markWordSize % calFactor) {
            markWordSize = markWordSize + calFactor - markWordSize % calFactor;
        }
        auto addWord = [&](size_t idx, uint64_t mask) {
            partLiveBytes[idx / (markWordSize / calFactor)].fetch_add(
                __builtin_popcountll(mask & kLiveBitMask));
        };
        addWord(maskInfo.headWordIdx, maskInfo.headMaskBits);
        if (maskInfo.tailWordCnt > 0) {
            const size_t lastWordIdx = maskInfo.headWordIdx + maskInfo.tailWordCnt;
            const size_t fullEnd = maskInfo.lastMaskBits == 0 ? lastWordIdx + 1 : lastWordIdx;
            for (size_t idx = maskInfo.headWordIdx + 1; idx < fullEnd; ++idx) {
                addWord(idx, ~static_cast<uint64_t>(0));
            }
            if (maskInfo.lastMaskBits != 0) {
                addWord(lastWordIdx, maskInfo.lastMaskBits);
            }
        }
        liveBytes.fetch_add(byteCnt);
    }

    explicit RegionBitmap(size_t regionSize)
        : liveBytes(0), wordCnt(regionSize / kRegionBytesPerWord)
    {}

    // Reset the bitmap state without exposing markWords/wordCnt to tests.
    // Keeping this operation on the carrier makes the concurrent invariant
    // independent of the number of words or any future pair packing.
    void Reset()
    {
        liveBytes.store(0, std::memory_order_relaxed);
        for (auto& part : partLiveBytes) {
            part.store(0, std::memory_order_relaxed);
        }
        const size_t words = wordCnt.load(std::memory_order_relaxed);
        for (size_t idx = 0; idx < words; ++idx) {
            markWords[idx].store(0, std::memory_order_relaxed);
        }
    }

    bool MarkBits(size_t start, size_t byteCnt, size_t regionSize, bool& incLive)
    {
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, byteCnt, maskInfo);
        // ZGC zBitMap.inline.hpp:60-83: one pair RMW decides strong ownership
        // and inc_live. The tail describes the same object's byte range; the
        // start pair remains the only arbitration point.
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(maskInfo.headMaskBits);
        const bool already = (old & maskInfo.strongStartBitMask) != 0;
        incLive = !already && (old & maskInfo.liveStartBitMask) == 0;
        SetTailMask(maskInfo, ~static_cast<uint64_t>(0));
        if (incLive) {
            AddLiveBytesForMask(maskInfo, byteCnt, regionSize);
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
        BitMaskInfo maskInfo;
        GetBitMaskInfo(start, byteCnt, maskInfo);
        const uint64_t old = markWords[maskInfo.headWordIdx].fetch_or(maskInfo.headMaskBits & kLiveBitMask);
        const bool already = (old & maskInfo.liveStartBitMask) != 0;
        incLive = !already;
        SetTailMask(maskInfo, kLiveBitMask);
        if (incLive) {
            AddLiveBytesForMask(maskInfo, byteCnt, regionSize);
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

    struct PreMaskInfo {
        int8_t partIndex;
        uint64_t mask;
        ssize_t StepSize;
        ssize_t index;
    };

    static void GetPreMaskInfo(size_t offset, size_t regionSize, PreMaskInfo& maskInfo)
    {
        const size_t pairBit = 2 * (offset / kMarkedBytesPerBit);
        maskInfo.index = pairBit / kBitsPerWord;
        size_t markWordSize = regionSize / kRegionBytesPerWord;
        uint8_t calFactor = factor > markWordSize ? markWordSize : factor;
        if (markWordSize % calFactor) {
            // The markWordSize needs to be rounded up to ensure it is divisible by calFactor.
            markWordSize = markWordSize + calFactor - markWordSize % calFactor;
        }
        maskInfo.partIndex = maskInfo.index / (markWordSize / calFactor) - 1;
        size_t bitIndex = pairBit % kBitsPerWord;
        maskInfo.mask = ((static_cast<uint64_t>(1) << bitIndex) - 1) & kLiveBitMask;
        maskInfo.StepSize = markWordSize / calFactor;
    }

    uint64_t GetPreLiveBytes(const PreMaskInfo& maskInfo)
    {
        uint64_t preLiveBits = 0;
        ssize_t partStartIndex = 0;
        int8_t partIndex = maskInfo.partIndex;
        while (partIndex >= 0) {
            preLiveBits += partLiveBytes[partIndex--];
            partStartIndex += maskInfo.StepSize;
        }
        ssize_t index = maskInfo.index;
        size_t liveBits = __builtin_popcountll(markWords[index].load() & maskInfo.mask & kLiveBitMask);

        if (index == partStartIndex) {
            return (preLiveBits + liveBits) * kMarkedBytesPerBit;
        }
        index--;
        while (index >= partStartIndex) {
            uint64_t makeBit = markWords[index].load();
            liveBits += __builtin_popcountll(makeBit & kLiveBitMask);
            index--;
        }
        return (preLiveBits + liveBits) * kMarkedBytesPerBit;
    }

    size_t GetLiveBytes() const { return liveBytes.load(std::memory_order_acquire); }

    size_t RecomputeLiveBytes() const
    {
        size_t liveBits = 0;
        size_t count = wordCnt.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i) {
            liveBits += static_cast<size_t>(
                __builtin_popcountll(markWords[i].load(std::memory_order_acquire) & kLiveBitMask));
        }
        return liveBits * kMarkedBytesPerBit;
    }
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

    // Geometry prefix-sum: only RegionInfo::GetPreLiveBytesInGhostRegion (ticket path).
    // Anchor: ops/design/ROUTE_DOMAIN.md §2.
    friend class RegionInfo;
    template<Generation G>
    uint64_t GetPreLiveBytes(MarkView<G> view, size_t offset, size_t regionSize)
    {
        RegionBitmap::PreMaskInfo maskInfo;
        RegionBitmap::GetPreMaskInfo(offset, regionSize, maskInfo);
        uint64_t liveBytes = 0;
        MarkFace& face = GetMarkFace();
        RegionBitmap* markBitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        if (face.epoch.load(std::memory_order_acquire) == view.GetEpoch() && markBitmap != nullptr) {
            liveBytes += markBitmap->GetPreLiveBytes(maskInfo);
        }
        if (G == Generation::Old && resurrectBitmap != nullptr) {
            liveBytes += resurrectBitmap->GetPreLiveBytes(maskInfo);
        }
        return liveBytes;
    }
};

struct RouteInfo {
    static constexpr uint32_t INVALID_VALUE = std::numeric_limits<uint32_t>::max();
    uintptr_t toRegion1StartAddress = 0;
    uint32_t toRegion1UsedBytes = 0;
    uint32_t toRegion2Idx = 0;
    RegionLifeId lifeId = 0;

    uintptr_t GetRoute(uint64_t preLiveBytes);

    void SetRouteInfo(uintptr_t to1, uint32_t to1used = 0, uint32_t to2 = INVALID_VALUE,
                      RegionLifeId life = 0)
    {
        toRegion1StartAddress = to1;
        toRegion1UsedBytes = to1used;
        toRegion2Idx = to2;
        lifeId = life;
    }
    void Clear()
    {
        toRegion1StartAddress = 0;
        toRegion1UsedBytes = 0;
        toRegion2Idx = INVALID_VALUE;
        lifeId = 0;
    }
    bool HasRoute() const { return toRegion1StartAddress != 0; }
    RegionLifeId GetLifeId() const { return lifeId; }
    uint32_t GetToRegion1UsedBytes() const { return toRegion1UsedBytes; }
    uint32_t GetToRegion2Idx() const { return toRegion2Idx; }
};
} // namespace MapleRuntime
#endif // MRT_LIVE_INFO_H
