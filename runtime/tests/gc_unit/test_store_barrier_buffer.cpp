// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <unordered_set>

#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private

#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

MAddress SlotAt(GcHeapFixture& fx, size_t i)
{
    return fx.heapStart + i * sizeof(void*);
}

} // namespace

GC_TEST(StoreBuf, FullAutoFlushKeepsEveryEntry)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    GC_EXPECT_TRUE(kBufferStoreBarriers);
    const size_t n = kStoreBarrierBufferLength + 1;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), rs);
    }
    GC_EXPECT_EQ(buf.Pending(), 1u);
    GC_EXPECT_EQ(rs.Size(), kStoreBarrierBufferLength);
    buf.Flush(rs);
    GC_EXPECT_TRUE(buf.IsEmpty());
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_EQ(drained.size(), n);
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(drained.count(SlotAt(fx, i + 8)) == 1);
    }
}

GC_TEST(StoreBuf, UnflushedPendingInvisibleToDrain)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.Add(slot, rs);
    GC_EXPECT_EQ(buf.Pending(), 1u);
    std::unordered_set<MAddress> lost;
    rs.DrainForMinor(lost);
    GC_EXPECT_EQ(lost.size(), 0u);
    GC_EXPECT_EQ(buf.Pending(), 1u);
}

GC_TEST(StoreBuf, FlushBeforeMinorDoesNotLoseEdges)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const size_t n = 7;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), rs);
    }
    buf.Flush(rs);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_EQ(drained.size(), n);
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(drained.count(SlotAt(fx, i + 8)) == 1);
    }
}

GC_TEST(StoreBuf, ThreadExitFlushRedeems)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 9);
    buf.Add(slot, rs);
    buf.Flush(rs);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_TRUE(drained.count(slot) == 1);
}

GC_TEST(StoreBuf, ReRememberDoesNotFightBuffer)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 10);
    buf.Add(slot, rs);
    rs.Record(slot);
    rs.Record(slot);
    GC_EXPECT_EQ(rs.Size(), 1u);
    buf.Flush(rs);
    GC_EXPECT_EQ(rs.Size(), 1u);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_TRUE(drained.count(slot) == 1);
    GC_EXPECT_EQ(drained.size(), 1u);
}
