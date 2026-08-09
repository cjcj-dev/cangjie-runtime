// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U4 — live bitmap + liveInfo0 snapshot timing (HotSpot test_zLiveMap.cpp shape).
// Defect anchor: installdomain BindLiveInfo0FromLiveIfNull; GetRoute reads ghost liveInfo0.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "gc_unittest.hpp"

using namespace MapleRuntime::GcUnit;

namespace {

// Extracted pure RegionBitmap mark/isMarked matching LiveInfo.h (no Heap/RegionInfo deps).
constexpr size_t kBitsPerByte = 8;
constexpr size_t kMarkedBytesPerBit = 8;
constexpr size_t kBitsPerWord = sizeof(uint64_t) * kBitsPerByte;

struct RegionBitmap {
    static constexpr uint8_t factor = 16;
    std::atomic<uint16_t> partLiveBytes[factor];
    std::atomic<size_t> liveBytes;
    std::atomic<size_t> wordCnt;
    std::atomic<uint64_t> markWords[0];

    static size_t GetRegionBitmapSize(size_t regionSize)
    {
        return sizeof(RegionBitmap) + ((regionSize / (kMarkedBytesPerBit * kBitsPerWord)) * sizeof(uint64_t));
    }

    explicit RegionBitmap(size_t regionSize) : liveBytes(0), wordCnt(regionSize / (kMarkedBytesPerBit * kBitsPerWord))
    {
        for (auto& p : partLiveBytes) {
            p.store(0, std::memory_order_relaxed);
        }
    }

    bool MarkBits(size_t start)
    {
        size_t headWordIdx = (start / kMarkedBytesPerBit) / kBitsPerWord;
        size_t headMaskBitStart = (start / kMarkedBytesPerBit) % kBitsPerWord;
        uint64_t mask = static_cast<uint64_t>(1) << headMaskBitStart;
        uint64_t old = markWords[headWordIdx].fetch_or(mask);
        bool wasMarked = (old & mask) != 0;
        if (!wasMarked) {
            liveBytes.fetch_add(kMarkedBytesPerBit);
        }
        return wasMarked;
    }

    bool IsMarked(size_t start) const
    {
        size_t headWordIdx = (start / kMarkedBytesPerBit) / kBitsPerWord;
        size_t headMaskBitStart = (start / kMarkedBytesPerBit) % kBitsPerWord;
        uint64_t mask = static_cast<uint64_t>(1) << headMaskBitStart;
        return (markWords[headWordIdx].load() & mask) != 0;
    }
};

struct LiveInfo {
    RegionBitmap* markBitmap = nullptr;
    RegionBitmap* resurrectBitmap = nullptr;

    bool IsSurvivedObject(size_t offset) const
    {
        return (markBitmap != nullptr && markBitmap->IsMarked(offset)) ||
            (resurrectBitmap != nullptr && resurrectBitmap->IsMarked(offset));
    }
};

// PrepareForwardableRegion snapshot: liveInfo0 = liveInfo (pointer share).
struct GhostSnapshot {
    LiveInfo* liveInfo = nullptr;
    LiveInfo* liveInfo0 = nullptr;

    void PrepareForwardable() { liveInfo0 = liveInfo; }

    void ClearLiveInfo()
    {
        // RegionInfo clear path nulls liveInfo after snapshot; ghost must remain.
        liveInfo = nullptr;
    }

    void BindLiveInfo0FromLiveIfNull()
    {
        if (liveInfo0 != nullptr) {
            return;
        }
        if (liveInfo == nullptr) {
            return;
        }
        liveInfo0 = liveInfo;
    }
};

RegionBitmap* AllocBitmap(size_t regionSize)
{
    size_t bytes = RegionBitmap::GetRegionBitmapSize(regionSize);
    void* mem = std::calloc(1, bytes);
    if (mem == nullptr) {
        std::abort();
    }
    return new (mem) RegionBitmap(regionSize);
}

void FreeBitmap(RegionBitmap* bm)
{
    if (bm != nullptr) {
        bm->~RegionBitmap();
        std::free(bm);
    }
}

} // namespace

// U4: mark then IsSurvivedObject; unmarked offset stays dead.
GC_TEST(LiveMap, MarkAndSurvive)
{
    constexpr size_t kRegion = 4096;
    RegionBitmap* bm = AllocBitmap(kRegion);
    LiveInfo live;
    live.markBitmap = bm;

    GC_EXPECT_FALSE(live.IsSurvivedObject(0));
    GC_EXPECT_FALSE(live.IsSurvivedObject(64));

    bool was = bm->MarkBits(64);
    GC_EXPECT_FALSE(was);
    GC_EXPECT_TRUE(live.IsSurvivedObject(64));
    GC_EXPECT_FALSE(live.IsSurvivedObject(0));
    GC_EXPECT_FALSE(live.IsSurvivedObject(128));

    // Second mark is idempotent (already marked).
    GC_EXPECT_TRUE(bm->MarkBits(64));
    GC_EXPECT_TRUE(live.IsSurvivedObject(64));

    FreeBitmap(bm);
}

// U4: liveInfo0 is a snapshot of liveInfo — clearing current must not drop ghost marks.
GC_TEST(LiveMap, LiveInfo0SnapshotSurvivesClear)
{
    constexpr size_t kRegion = 4096;
    RegionBitmap* bm = AllocBitmap(kRegion);
    LiveInfo liveBody;
    liveBody.markBitmap = bm;
    bm->MarkBits(256);

    GhostSnapshot g;
    g.liveInfo = &liveBody;
    g.PrepareForwardable();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(g.liveInfo0), reinterpret_cast<uintptr_t>(&liveBody));
    GC_EXPECT_TRUE(g.liveInfo0->IsSurvivedObject(256));

    g.ClearLiveInfo();
    // Ghost still answers survive for the snapshotted marks.
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(g.liveInfo), 0u);
    GC_EXPECT_TRUE(g.liveInfo0 != nullptr);
    GC_EXPECT_TRUE(g.liveInfo0->IsSurvivedObject(256));

    FreeBitmap(bm);
}

// U4: installdomain bind — late mark path binds null ghost from current live.
GC_TEST(LiveMap, BindLiveInfo0AfterLateMark)
{
    constexpr size_t kRegion = 4096;
    RegionBitmap* bm = AllocBitmap(kRegion);
    LiveInfo liveBody;
    liveBody.markBitmap = bm;
    bm->MarkBits(8);

    GhostSnapshot g;
    // PrepareForwardable saw null liveInfo → ghost stays null (installdomain defect shape).
    g.liveInfo = nullptr;
    g.PrepareForwardable();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(g.liveInfo0), 0u);

    // Mark path later creates liveInfo and paints.
    g.liveInfo = &liveBody;
    g.BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(g.liveInfo0), reinterpret_cast<uintptr_t>(&liveBody));
    GC_EXPECT_TRUE(g.liveInfo0->IsSurvivedObject(8));

    FreeBitmap(bm);
}

// U4: null markBitmap ⇒ never survived (GetRoute domain reject).
GC_TEST(LiveMap, NullBitmapNeverSurvived)
{
    LiveInfo live;
    GC_EXPECT_FALSE(live.IsSurvivedObject(0));
    GC_EXPECT_FALSE(live.IsSurvivedObject(100));
}
