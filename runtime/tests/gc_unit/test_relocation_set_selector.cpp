// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocationSetSelector.hpp"
#include "gc_unittest.hpp"

#include <algorithm>
#include <set>
#include <vector>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

RelocRegionDesc Make(uint32_t id, size_t live, size_t cap, RelocRegionKind kind = RelocRegionKind::Small)
{
    RelocRegionDesc d;
    d.id = id;
    d.liveBytes = live;
    d.capacity = cap;
    d.kind = kind;
    return d;
}

bool ContainsId(const RelocSelectResult& r, uint32_t id)
{
    return std::find(r.selectedIds.begin(), r.selectedIds.end(), id) != r.selectedIds.end();
}

GC_TEST(RelocationSetSelector, EmptySet)
{
    std::vector<RelocRegionDesc> in;
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_EQ(r.selectedIds.size(), 0u);
}

GC_TEST(RelocationSetSelector, AllSparseSelected)
{
    const size_t cap = 4096;
    std::vector<RelocRegionDesc> in;
    for (uint32_t i = 0; i < 16; ++i) {
        in.push_back(Make(i, 64, cap));
    }
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_EQ(r.selectedIds.size(), 16u);
}

GC_TEST(RelocationSetSelector, FragmentationLimitStopsPrefix)
{
    const size_t cap = kRelocationMaxSmallRegionBytes;
    std::vector<RelocRegionDesc> in;
    // Eight almost-empty 128KB pages pack tightly (selected).
    for (uint32_t i = 0; i < 8; ++i) {
        in.push_back(Make(i, 128, cap));
    }
    // Then many nearly-full pages: incremental reclaimable drops under 25%.
    for (uint32_t i = 8; i < 24; ++i) {
        in.push_back(Make(i, cap - 1, cap));
    }
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_TRUE(r.selectedIds.size() >= 8u);
    GC_EXPECT_TRUE(r.selectedIds.size() < 24u);
    for (uint32_t i = 0; i < 8; ++i) {
        GC_EXPECT_TRUE(ContainsId(r, i));
    }
}

GC_TEST(RelocationSetSelector, EqualLiveBytesStableOrder)
{
    const size_t cap = 4096;
    std::vector<RelocRegionDesc> in;
    in.push_back(Make(10, 80, cap));
    in.push_back(Make(11, 80, cap));
    in.push_back(Make(12, 80, cap));
    in.push_back(Make(13, 80, cap));
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_EQ(r.selectedIds.size(), 4u);
    GC_EXPECT_EQ(r.selectedIds[0], 10u);
    GC_EXPECT_EQ(r.selectedIds[1], 11u);
    GC_EXPECT_EQ(r.selectedIds[2], 12u);
    GC_EXPECT_EQ(r.selectedIds[3], 13u);
}

GC_TEST(RelocationSetSelector, LargeRegionsNeverSelected)
{
    const size_t cap = 256 * 1024;
    std::vector<RelocRegionDesc> in;
    in.push_back(Make(1, 64, 4096, RelocRegionKind::Small));
    in.push_back(Make(2, 64, 4096, RelocRegionKind::Small));
    in.push_back(Make(99, 100, cap, RelocRegionKind::Large));
    in.push_back(Make(100, 200, cap, RelocRegionKind::Large));
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_FALSE(ContainsId(r, 99u));
    GC_EXPECT_FALSE(ContainsId(r, 100u));
    GC_EXPECT_TRUE(ContainsId(r, 1u));
    GC_EXPECT_TRUE(ContainsId(r, 2u));
}

GC_TEST(RelocationSetSelector, PreFilterDropsLowGarbage)
{
    const size_t cap = 4096;
    std::vector<RelocRegionDesc> in;
    // garbage = 100 <= 4096*0.25 — pre-filter rejects (inline.hpp:81-83)
    in.push_back(Make(1, cap - 100, cap));
    in.push_back(Make(2, 64, cap));
    in.push_back(Make(3, 64, cap));
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_FALSE(ContainsId(r, 1u));
}

// markwater2: zGeneration.cpp:211-213 — is_allocating pages are not candidates.
GC_TEST(RelocationSetSelector, AllocatingPagesNeverSelected)
{
    const size_t cap = 4096;
    std::vector<RelocRegionDesc> in;
    RelocRegionDesc allocating = Make(7, 64, cap);
    allocating.allocating = true;
    in.push_back(allocating);
    in.push_back(Make(8, 64, cap));
    in.push_back(Make(9, 64, cap));
    const RelocSelectResult r = SelectRelocationSet(in);
    GC_EXPECT_FALSE(ContainsId(r, 7u));
    GC_EXPECT_TRUE(ContainsId(r, 8u));
    GC_EXPECT_TRUE(ContainsId(r, 9u));
}

// ZRelocationSetSelectorGroup::semi_sort (zRelocationSetSelector.cpp:84).
// Deliberately reverse live bytes inside a bucket: full sorting changes this result.
GC_TEST(RelocationSetSelector, SemiSortUsesPartitionFingers)
{
    const size_t cap = kRelocationMaxSmallRegionBytes;
    std::vector<RelocRegionDesc> in{
        Make(1, 191, cap), Make(2, 127, cap), Make(3, 128, cap), Make(4, 64, cap),
        Make(5, 0, cap), Make(6, 192, cap)};
    const RelocSelectResult r = SelectRelocationSet(in);
    const std::vector<uint32_t> expected{5, 2, 4, 1, 3, 6};
    GC_EXPECT_TRUE(r.selectedIds == expected);
}

// ZRelocationSetSelectorGroup::partition_index (zRelocationSetSelector.cpp:77).
// Page-size adaptation: order by occupancy bucket, not absolute live bytes.
GC_TEST(RelocationSetSelector, PartitionUsesEachPageCapacity)
{
    std::vector<RelocRegionDesc> in{
        Make(1, 1024, 4096), Make(2, 2048, 16384), Make(3, 1536, 12288)};
    const RelocSelectResult r = SelectRelocationSet(in);
    const std::vector<uint32_t> expected{2, 3, 1};
    GC_EXPECT_TRUE(r.selectedIds == expected);
}

// ZRelocationSetSelectorGroup::select_inner (zRelocationSetSelector.cpp:162-174).
// Two pages require two destinations; a third amortizes the same two destinations.
GC_TEST(RelocationSetSelector, SelectionContinuesPastUnprofitablePrefix)
{
    const size_t cap = kRelocationMaxSmallRegionBytes;
    const size_t live = 60000;
    std::vector<RelocRegionDesc> in{Make(1, live, cap), Make(2, live, cap)};
    GC_EXPECT_TRUE(SelectRelocationSet(in).selectedIds.empty());
    in.push_back(Make(3, live, cap));
    const RelocSelectResult r = SelectRelocationSet(in);
    const std::vector<uint32_t> expected{1, 2, 3};
    GC_EXPECT_TRUE(r.selectedIds == expected);
}

} // namespace
