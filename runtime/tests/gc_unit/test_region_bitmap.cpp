// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Eth: RegionBitmap geometry (JDK test_zBitMap shape, no GPL).
// Anchors: LiveInfo.h RegionBitmap; RegionInfo.h nullroute bitCover=
//   wordCnt * kMarkedBytesPerBit * kBitsPerWord (8 * 64 = 512 bytes/word).
// iorfix excluded bitCover OOB family from the route bug; this suite watches geometry.

#include <cstdint>
#include <cstring>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// bitCover for a unit-sized region: every address offset in [0, regionSize) must be
// representable when markBitmap is fully allocated for that regionSize.
GC_TEST(RegionBitmap, BitCoverMatchesRegionSize)
{
    GcHeapFixture fx;
    size_t regionSize = fx.region0->GetRegionSize();
    GC_EXPECT_TRUE(regionSize > 0);

    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t wordCnt = bm->wordCnt.load();
    size_t bitCover = wordCnt * kMarkedBytesPerBit * kBitsPerWord;
    // Geometry: mark words cover the whole region (iorfix OOB family).
    GC_EXPECT_TRUE(bitCover >= regionSize);
    // Last byte of region is in-range for IsMarked/MarkBits.
    size_t lastOff = regionSize - 8;
    GC_EXPECT_FALSE(bm->IsMarked(lastOff));
    GC_EXPECT_FALSE(bm->MarkBits(lastOff, 8, regionSize));
    GC_EXPECT_TRUE(bm->IsMarked(lastOff));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// GetRegionBitmapSize is a pure function of regionSize (allocation footprint).
GC_TEST(RegionBitmap, GetRegionBitmapSizeMonotonic)
{
    size_t s1 = RegionBitmap::GetRegionBitmapSize(4096);
    size_t s2 = RegionBitmap::GetRegionBitmapSize(8192);
    size_t s3 = RegionBitmap::GetRegionBitmapSize(65536);
    GC_EXPECT_TRUE(s1 > sizeof(RegionBitmap));
    GC_EXPECT_TRUE(s2 > s1);
    GC_EXPECT_TRUE(s3 > s2);
}

// MarkBits idempotent + disjoint offsets (zBitMap set-pair spirit without GPL).
GC_TEST(RegionBitmap, MarkBitsIdempotentAndDisjoint)
{
    GcHeapFixture fx;
    size_t regionSize = fx.region0->GetRegionSize();
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);

    GC_EXPECT_FALSE(bm->MarkBits(0, 8, regionSize));
    GC_EXPECT_TRUE(bm->MarkBits(0, 8, regionSize));
    GC_EXPECT_FALSE(bm->MarkBits(64, 8, regionSize));
    GC_EXPECT_TRUE(bm->IsMarked(0));
    GC_EXPECT_TRUE(bm->IsMarked(64));
    GC_EXPECT_FALSE(bm->IsMarked(128));

    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// o2mark: neighbor bit in the same head word must not make MarkBits return "already"
// without setting the object start bit (IsMarkedObject CHECK family).
GC_TEST(RegionBitmap, MarkBitsAlreadyIsStartBitOnly)
{
    constexpr size_t kBig = 65536;
    size_t bytes = RegionBitmap::GetRegionBitmapSize(kBig);
    void* mem = std::calloc(1, bytes);
    GC_EXPECT_TRUE(mem != nullptr);
    auto* bm = new (mem) RegionBitmap(kBig);

    // Neighbor object at offset 8 (bit 1).
    GC_EXPECT_FALSE(bm->MarkBits(8, 8, kBig));
    GC_EXPECT_TRUE(bm->IsMarked(8));
    GC_EXPECT_FALSE(bm->IsMarked(0));

    // Large mark at offset 0 spans bit 0 and bit 1. Old already-test used any head-mask
    // bit ⇒ returned true without setting bit 0. Start-bit already ⇒ must write bit 0.
    bool already = bm->MarkBits(0, 16, kBig);
    GC_EXPECT_FALSE(already);
    GC_EXPECT_TRUE(bm->IsMarked(0));
    GC_EXPECT_TRUE(bm->IsMarked(8));

    // True already: start bit set ⇒ second MarkBits is idempotent.
    GC_EXPECT_TRUE(bm->MarkBits(0, 16, kBig));
    GC_EXPECT_TRUE(bm->IsMarked(0));

    bm->~RegionBitmap();
    std::free(bm);
}

// Offset near region end (65504/65520 family when region is 64KiB): still in bitCover.
GC_TEST(RegionBitmap, NearEndOffsetsInCover)
{
    // Synthetic 64KiB region bitmap (product large-region shape) without needing 64 units.
    constexpr size_t kBig = 65536;
    size_t bytes = RegionBitmap::GetRegionBitmapSize(kBig);
    void* mem = std::calloc(1, bytes);
    GC_EXPECT_TRUE(mem != nullptr);
    auto* bm = new (mem) RegionBitmap(kBig);
    size_t wordCnt = bm->wordCnt.load();
    size_t bitCover = wordCnt * kMarkedBytesPerBit * kBitsPerWord;
    GC_EXPECT_TRUE(bitCover >= kBig);
    for (size_t off : {size_t(65504), size_t(65520), size_t(65528)}) {
        if (off + 8 <= kBig) {
            GC_EXPECT_FALSE(bm->IsMarked(off));
            (void)bm->MarkBits(off, 8, kBig);
            GC_EXPECT_TRUE(bm->IsMarked(off));
        }
    }
    bm->~RegionBitmap();
    std::free(bm);
}
