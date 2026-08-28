// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Eth: RegionBitmap geometry (JDK test_zBitMap shape, no GPL).
// Anchors: LiveInfo.h RegionBitmap; one pair word covers
//   kRegionBytesPerWord = 32 * 8 = 256 bytes.
// iorfix excluded bitCover OOB family from the route bug; this suite watches geometry.

#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

bool GeometryUnderallocArm()
{
    const char* value = std::getenv("MRT_GC_GEOMETRY_UNDERALLOC");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

} // namespace

// Shared half of test_zBitMap.cpp::test_set_pair_unset.  A Cangjie mark bit
// covers eight bytes, so a strongly marked 16-byte object is the same two-bit
// transition.  MarkBits returns "already marked" (the inverse polarity of
// ZBitMap::par_set_bit_pair's success return), and live bytes are the inc_live
// witness.
GC_TEST(ZBitMapPort, StrongPairUnset)
{
    constexpr size_t kBitsPerWord = sizeof(uintptr_t) * 8;
    // RegionBitmap requires at least one complete 64-bit mark word.  Keep a
    // valid backing bitmap while `bitSize` preserves ZGC's logical test range.
    constexpr size_t kBackingRegionSize = 4096;
    const size_t bitSizes[] = { 2, 62, 64, 66, 126, 128 };
    for (size_t bitSize : bitSizes) {
        for (size_t i = 0; i < bitSize - 1; ++i) {
            if ((i + 1) % kBitsPerWord == 0) {
                continue;
            }
            RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kBackingRegionSize);
            const size_t offset = i * kMarkedBytesPerBit;
            GC_EXPECT_FALSE(bitmap->MarkBits(offset, 2 * kMarkedBytesPerBit, kBackingRegionSize));
            GC_EXPECT_TRUE(bitmap->IsMarked(offset));
            GC_EXPECT_TRUE(bitmap->IsMarked(offset + kMarkedBytesPerBit));
            GC_EXPECT_EQ(bitmap->GetLiveBytes(), 2 * kMarkedBytesPerBit);
            GC_EXPECT_EQ(bitmap->RecomputeLiveBytes(), 2 * kMarkedBytesPerBit);
            GcHeapFixture::FreePlantedBitmap(bitmap);
        }
    }
}

// Shared half of test_zBitMap.cpp::test_set_pair_set.  Once every bit is set,
// setting any pair must report "already", leave both bits set, and not account
// live bytes a second time.
GC_TEST(ZBitMapPort, StrongPairSet)
{
    constexpr size_t kBitsPerWord = sizeof(uintptr_t) * 8;
    // See StrongPairUnset: backing geometry is not part of the pair invariant.
    constexpr size_t kBackingRegionSize = 4096;
    const size_t bitSizes[] = { 2, 62, 64, 66, 126, 128 };
    for (size_t bitSize : bitSizes) {
        const size_t logicalSize = bitSize * kMarkedBytesPerBit;
        RegionBitmap* bitmap = GcHeapFixture::AllocPlantedBitmap(kBackingRegionSize);
        GC_EXPECT_FALSE(bitmap->MarkBits(0, logicalSize, kBackingRegionSize));
        GC_EXPECT_EQ(bitmap->GetLiveBytes(), logicalSize);

        for (size_t i = 0; i < bitSize - 1; ++i) {
            if ((i + 1) % kBitsPerWord == 0) {
                continue;
            }
            const size_t offset = i * kMarkedBytesPerBit;
            GC_EXPECT_TRUE(bitmap->MarkBits(offset, 2 * kMarkedBytesPerBit, kBackingRegionSize));
            GC_EXPECT_TRUE(bitmap->IsMarked(offset));
            GC_EXPECT_TRUE(bitmap->IsMarked(offset + kMarkedBytesPerBit));
            GC_EXPECT_EQ(bitmap->GetLiveBytes(), logicalSize);
        }
        GcHeapFixture::FreePlantedBitmap(bitmap);
    }
}

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
    const size_t expectedWords = regionSize / RegionBitmap::kRegionBytesPerWord;
    if (GeometryUnderallocArm()) {
        // Fault arm: preserve the full allocation but narrow the observable
        // geometry quantity.  This keeps every unrelated test on the normal
        // product bitmap while making the two geometry assertions fail
        // precisely with words=128 expected=256 for the 64KiB fixture.
        bm->wordCnt.store(expectedWords / 2);
        wordCnt = bm->wordCnt.load();
    }
    std::fprintf(stderr, "DETAIL geometry words=%zu expected=%zu\n", wordCnt, expectedWords);
    size_t bitCover = wordCnt * RegionBitmap::kRegionBytesPerWord;
    // Geometry: pair words cover the whole region (iorfix OOB family).
    GC_EXPECT_TRUE(bitCover >= regionSize);
    GC_EXPECT_EQ(wordCnt, expectedWords);
    // Positive control for the geometry尺: a known one-half allocation must
    // be rejected by this same pair invariant, so the old 512B/word formula
    // cannot mask an under-allocation.
    const size_t underAllocatedWords = wordCnt / 2;
    GC_EXPECT_TRUE(underAllocatedWords * RegionBitmap::kRegionBytesPerWord < regionSize);
    // Last-byte probing is valid only with the full backing geometry.  The
    // underallocation arm stops after the geometry invariant so it cannot
    // perturb neighboring tests with an out-of-bounds mark-word access.
    if (!GeometryUnderallocArm()) {
        size_t lastOff = regionSize - 8;
        GC_EXPECT_FALSE(bm->IsMarked(lastOff));
        GC_EXPECT_FALSE(bm->MarkBits(lastOff, 8, regionSize));
        GC_EXPECT_TRUE(bm->IsMarked(lastOff));
    }

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
    const size_t expectedWords = kBig / RegionBitmap::kRegionBytesPerWord;
    if (GeometryUnderallocArm()) {
        bm->wordCnt.store(expectedWords / 2);
        wordCnt = bm->wordCnt.load();
    }
    std::fprintf(stderr, "DETAIL geometry_near_end words=%zu expected=%zu\n", wordCnt, expectedWords);
    size_t bitCover = wordCnt * RegionBitmap::kRegionBytesPerWord;
    GC_EXPECT_TRUE(bitCover >= kBig);
    GC_EXPECT_EQ(wordCnt, expectedWords);
    if (!GeometryUnderallocArm()) {
        for (size_t off : {size_t(65504), size_t(65520), size_t(65528)}) {
            if (off + 8 <= kBig) {
                GC_EXPECT_FALSE(bm->IsMarked(off));
                (void)bm->MarkBits(off, 8, kBig);
                GC_EXPECT_TRUE(bm->IsMarked(off));
            }
        }
    }
    bm->~RegionBitmap();
    std::free(bm);
}
