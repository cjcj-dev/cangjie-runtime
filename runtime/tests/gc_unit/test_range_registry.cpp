// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_unittest.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Heap/Allocator/RangeRegistry.h"

namespace MapleRuntime {
namespace GcUnit {

namespace {
void ExpectRange(const Range& actual, uintptr_t start, size_t size)
{
    GC_EXPECT_FALSE(actual.IsNull());
    GC_EXPECT_EQ(actual.Start(), start);
    GC_EXPECT_EQ(actual.Size(), size);
    GC_EXPECT_EQ(actual.End(), start + size);
}

void ExpectSnapshot(const RangeRegistry& registry, const std::vector<Range>& expected)
{
    const std::vector<Range> actual = registry.Snapshot();
    GC_EXPECT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        GC_EXPECT_TRUE(actual[i] == expected[i]);
        if (i != 0) {
            GC_EXPECT_TRUE(actual[i - 1].End() < actual[i].Start());
        }
    }
}

struct CallbackLog {
    size_t handOuts{ 0 };
    size_t handBacks{ 0 };
    size_t grows{ 0 };
    size_t shrinks{ 0 };
    size_t handedOutBytes{ 0 };
    size_t handedBackBytes{ 0 };
};

void NoteHandOut(const Range& range, void* context)
{
    auto* log = static_cast<CallbackLog*>(context);
    ++log->handOuts;
    log->handedOutBytes += range.Size();
}

void NoteHandBack(const Range& range, void* context)
{
    auto* log = static_cast<CallbackLog*>(context);
    ++log->handBacks;
    log->handedBackBytes += range.Size();
}

void NoteGrow(const Range& from, const Range& to, void* context)
{
    auto* log = static_cast<CallbackLog*>(context);
    GC_EXPECT_TRUE(from.IsValid());
    GC_EXPECT_TRUE(to.IsValid());
    GC_EXPECT_TRUE(to.Size() > from.Size());
    ++log->grows;
}

void NoteShrink(const Range& from, const Range& to, void* context)
{
    auto* log = static_cast<CallbackLog*>(context);
    GC_EXPECT_TRUE(from.IsValid());
    GC_EXPECT_TRUE(to.IsValid());
    GC_EXPECT_TRUE(to.Size() < from.Size());
    ++log->shrinks;
}

Range ModelClaim(std::vector<bool>& free, uintptr_t base, size_t size, bool high)
{
    struct Run {
        size_t first;
        size_t length;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < free.size();) {
        if (!free[i]) {
            ++i;
            continue;
        }
        const size_t first = i;
        while (i < free.size() && free[i]) {
            ++i;
        }
        runs.push_back(Run{ first, i - first });
    }

    auto take = [&](const Run& run) {
        const size_t first = high ? run.first + run.length - size : run.first;
        for (size_t i = first; i < first + size; ++i) {
            free[i] = false;
        }
        return Range(base + first, size);
    };
    if (high) {
        for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
            if (run->length >= size) {
                return take(*run);
            }
        }
    } else {
        for (const Run& run : runs) {
            if (run.length >= size) {
                return take(run);
            }
        }
    }
    return Range();
}

void ExpectModel(const RangeRegistry& registry, const std::vector<bool>& free, uintptr_t base)
{
    std::vector<Range> expected;
    size_t total = 0;
    for (size_t i = 0; i < free.size();) {
        if (!free[i]) {
            ++i;
            continue;
        }
        const size_t first = i;
        while (i < free.size() && free[i]) {
            ++i;
        }
        expected.push_back(Range(base + first, i - first));
        total += i - first;
    }
    ExpectSnapshot(registry, expected);
    GC_EXPECT_EQ(registry.TotalSize(), total);
}
} // namespace

GC_TEST(RangeValue, SplitGrowAndPartitionPreserveEveryByte)
{
    Range range(100, 80);
    ExpectRange(range.FirstPart(30), 100, 30);
    ExpectRange(range.LastPart(30), 130, 50);
    ExpectRange(range.Partition(20, 17), 120, 17);

    const Range low = range.ShrinkFromFront(20);
    ExpectRange(low, 100, 20);
    ExpectRange(range, 120, 60);
    const Range high = range.ShrinkFromBack(15);
    ExpectRange(high, 165, 15);
    ExpectRange(range, 120, 45);
    GC_EXPECT_TRUE(range.GrowFromFront(low.Size()));
    GC_EXPECT_TRUE(range.GrowFromBack(high.Size()));
    ExpectRange(range, 100, 80);
}

GC_TEST(RangeRegistryMerge, AdjacentInsertMergesBothNeighbours)
{
    RangeRegistry registry;
    GC_EXPECT_TRUE(registry.RegisterRange(Range(100, 10)));
    GC_EXPECT_TRUE(registry.RegisterRange(Range(130, 10)));
    GC_EXPECT_TRUE(registry.Insert(Range(110, 20)));
    ExpectSnapshot(registry, { Range(100, 40) });
    GC_EXPECT_TRUE(registry.IsContiguous());

    GC_EXPECT_FALSE(registry.Insert(Range(105, 2)));
    ExpectSnapshot(registry, { Range(100, 40) });
}

GC_TEST(RangeRegistryMerge, HandOutAndBackCallbacksKeepTheLedgerReversible)
{
    RangeRegistry registry;
    CallbackLog log;
    RangeRegistry::Callbacks callbacks;
    callbacks.prepareForHandOut = NoteHandOut;
    callbacks.prepareForHandBack = NoteHandBack;
    callbacks.grow = NoteGrow;
    callbacks.shrink = NoteShrink;
    callbacks.context = &log;
    registry.RegisterCallbacks(callbacks);
    GC_EXPECT_TRUE(registry.RegisterRange(Range(200, 100)));

    const size_t before = registry.TotalSize();
    const Range handed = registry.ClaimLow(25);
    GC_EXPECT_EQ(before, registry.TotalSize() + handed.Size());
    GC_EXPECT_TRUE(registry.Insert(handed));
    GC_EXPECT_EQ(registry.TotalSize(), before);
    ExpectSnapshot(registry, { Range(200, 100) });

    GC_EXPECT_EQ(log.handOuts, 1u);
    GC_EXPECT_EQ(log.handBacks, 1u);
    GC_EXPECT_EQ(log.grows, 1u);
    GC_EXPECT_EQ(log.shrinks, 1u);
    GC_EXPECT_EQ(log.handedOutBytes, handed.Size());
    GC_EXPECT_EQ(log.handedBackBytes, handed.Size());
}

GC_TEST(RangeRegistryDirection, LowAndHighClaimsUseOppositeAddressEnds)
{
    RangeRegistry lowRegistry;
    GC_EXPECT_TRUE(lowRegistry.RegisterRange(Range(100, 50)));
    GC_EXPECT_TRUE(lowRegistry.RegisterRange(Range(300, 80)));
    ExpectRange(lowRegistry.ClaimLow(20), 100, 20);
    ExpectSnapshot(lowRegistry, { Range(120, 30), Range(300, 80) });

    RangeRegistry highRegistry;
    GC_EXPECT_TRUE(highRegistry.RegisterRange(Range(100, 50)));
    GC_EXPECT_TRUE(highRegistry.RegisterRange(Range(300, 80)));
    ExpectRange(highRegistry.ClaimHigh(20), 360, 20);
    ExpectSnapshot(highRegistry, { Range(100, 50), Range(300, 60) });
}

GC_TEST(RangeRegistryFailure, LowNoFitReturnsNullAndPreservesLedger)
{
    RangeRegistry registry;
    GC_EXPECT_TRUE(registry.RegisterRange(Range(100, 4)));
    GC_EXPECT_TRUE(registry.RegisterRange(Range(200, 7)));
    const Range actual = registry.ClaimLow(8);
    GC_EXPECT_TRUE(actual.IsNull());
    ExpectSnapshot(registry, { Range(100, 4), Range(200, 7) });
}

GC_TEST(RangeRegistryFailure, HighNoFitReturnsNullAndPreservesLedger)
{
    RangeRegistry registry;
    GC_EXPECT_TRUE(registry.RegisterRange(Range(100, 4)));
    GC_EXPECT_TRUE(registry.RegisterRange(Range(200, 7)));
    const Range actual = registry.ClaimHigh(8);
    GC_EXPECT_TRUE(actual.IsNull());
    ExpectSnapshot(registry, { Range(100, 4), Range(200, 7) });
}

GC_TEST(RangeRegistryFailure, ZeroSizeReturnsNullAndPreservesLedger)
{
    RangeRegistry registry;
    GC_EXPECT_TRUE(registry.RegisterRange(Range(100, 8)));
    GC_EXPECT_TRUE(registry.ClaimLow(0).IsNull());
    GC_EXPECT_TRUE(registry.ClaimHigh(0).IsNull());
    ExpectSnapshot(registry, { Range(100, 8) });
}

GC_TEST(RangeRegistryDirection, FixedRandomSequenceMatchesIndependentByteModel)
{
    constexpr uintptr_t base = 1000;
    constexpr size_t cells = 512;
    RangeRegistry registry;
    std::vector<bool> free(cells, false);
    const std::vector<Range> initial = { Range(base + 16, 96), Range(base + 176, 112), Range(base + 352, 128) };
    for (const Range& range : initial) {
        GC_EXPECT_TRUE(registry.RegisterRange(range));
        for (uintptr_t address = range.Start(); address < range.End(); ++address) {
            free[address - base] = true;
        }
    }
    ExpectModel(registry, free, base);

    uint32_t state = 0x5eed1234U;
    for (size_t operation = 0; operation < 72; ++operation) {
        state = state * 1664525U + 1013904223U;
        const bool high = (state & 1U) != 0;
        const size_t size = 1U + ((state >> 8U) % 4U);
        const Range expected = ModelClaim(free, base, size, high);
        const Range actual = high ? registry.ClaimHigh(size) : registry.ClaimLow(size);
        GC_EXPECT_TRUE(actual == expected);
        ExpectModel(registry, free, base);
    }
}

} // namespace GcUnit
} // namespace MapleRuntime
