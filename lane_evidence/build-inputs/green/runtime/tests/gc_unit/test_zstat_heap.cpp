// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// zStat.cpp:1703-2036 — the per-generation heap account fed by
// ZPageAllocatorStats at the six sample points. ZGC has no dedicated gtest;
// these pin the account arithmetic the director and the report read back.
#include "gc_unittest.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zPageAllocator.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
constexpr size_t MBYTE = 1024 * 1024;

ZPageAllocatorStats MakeStats(size_t capacity, size_t used, size_t usedGeneration, size_t freed, size_t promoted,
                              size_t compacted)
{
    return ZPageAllocatorStats(0, 1024 * MBYTE, 768 * MBYTE, capacity, used, used, used, usedGeneration, freed,
                               promoted, compacted, 0);
}

GC_TEST(ZStat, HeapAccountSixSamplePoints)
{
    ZStatHeap heap;
    heap.AtInitialize(0, 1024 * MBYTE);
    // Collection opens at 100M used, 100M of it young.
    heap.AtCollectionStart(MakeStats(512 * MBYTE, 100 * MBYTE, 100 * MBYTE, 0, 0, 0));
    heap.AtMarkStart(MakeStats(512 * MBYTE, 120 * MBYTE, 120 * MBYTE, 0, 0, 0));
    heap.AtMarkEnd(MakeStats(512 * MBYTE, 150 * MBYTE, 150 * MBYTE, 0, 0, 0));
    heap.AtRelocateStart(MakeStats(512 * MBYTE, 160 * MBYTE, 160 * MBYTE, 40 * MBYTE, 5 * MBYTE, 10 * MBYTE));
    heap.AtRelocateEnd(MakeStats(512 * MBYTE, 90 * MBYTE, 90 * MBYTE, 40 * MBYTE, 5 * MBYTE, 10 * MBYTE), true);

    GC_EXPECT_TRUE(heap.UsedAtCollectionStart() == 100 * MBYTE);
    GC_EXPECT_TRUE(heap.UsedAtMarkStart() == 120 * MBYTE);
    GC_EXPECT_TRUE(heap.UsedAtRelocateEnd() == 90 * MBYTE);
    GC_EXPECT_TRUE(heap.UsedAtCollectionEnd() == 90 * MBYTE);
    // mutator_allocated(B) = used_gen(B) - used_gen(mark_start) + freed - relocated
    GC_EXPECT_TRUE(heap.AllocatedAtMarkEnd() == 30 * MBYTE);
    // reclaimed = freed - relocated(compacted) - promoted
    GC_EXPECT_TRUE(heap.ReclaimedAtRelocateEnd() == 25 * MBYTE);
    // record_stats=true: the reclaimed sequence accepted one sample.
    GC_EXPECT_TRUE(heap.ReclaimedAvg() > 25 * MBYTE - 1 && heap.ReclaimedAvg() < 25 * MBYTE + 1);
    ZStatHeapStats stats = heap.Stats();
    GC_EXPECT_TRUE(stats.usedAtRelocateEnd == 90 * MBYTE);
    GC_EXPECT_TRUE(stats.reclaimedAverage > 0.0);
}

GC_TEST(ZStat, HeapAccountRecordStatsGatesSequence)
{
    ZStatHeap heap;
    heap.AtInitialize(0, 1024 * MBYTE);
    heap.AtCollectionStart(MakeStats(512 * MBYTE, 100 * MBYTE, 100 * MBYTE, 0, 0, 0));
    heap.AtMarkStart(MakeStats(512 * MBYTE, 100 * MBYTE, 100 * MBYTE, 0, 0, 0));
    heap.AtMarkEnd(MakeStats(512 * MBYTE, 100 * MBYTE, 100 * MBYTE, 0, 0, 0));
    heap.AtRelocateStart(MakeStats(512 * MBYTE, 100 * MBYTE, 100 * MBYTE, 64 * MBYTE, 0, 0));
    // record_stats=false: the reclaimed average must not take the sample.
    heap.AtRelocateEnd(MakeStats(512 * MBYTE, 36 * MBYTE, 36 * MBYTE, 64 * MBYTE, 0, 0), false);
    GC_EXPECT_TRUE(heap.ReclaimedAtRelocateEnd() == 64 * MBYTE);
    GC_EXPECT_TRUE(heap.ReclaimedAvg() < 1.0); // denorm floor only
}
}
