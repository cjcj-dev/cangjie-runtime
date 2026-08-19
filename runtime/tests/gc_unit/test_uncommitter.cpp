// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/Uncommitter.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(Uncommitter, ParseDelayDefaultAndOff)
{
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs(nullptr), Uncommitter::kDefaultDelayNs);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("0"), 0ULL);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("0s"), 0ULL);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("20s"), 20ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("20"), 20ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ParseDelayNs("300s"), 300ULL * SECOND_TO_NANO_SECOND);
}

GC_TEST(Uncommitter, TickIsMinDelayOverTenAnd30s)
{
    GC_EXPECT_EQ(Uncommitter::ComputeTickNs(0), 0ULL);
    GC_EXPECT_EQ(Uncommitter::ComputeTickNs(20ULL * SECOND_TO_NANO_SECOND), 2ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ComputeTickNs(300ULL * SECOND_TO_NANO_SECOND), 30ULL * SECOND_TO_NANO_SECOND);
    GC_EXPECT_EQ(Uncommitter::ComputeTickNs(600ULL * SECOND_TO_NANO_SECOND), 30ULL * SECOND_TO_NANO_SECOND);
}

GC_TEST(Uncommitter, MinCapacityIsLivePlusYoungReserve)
{
    GC_EXPECT_EQ(Uncommitter::MinCapacity(10 * MB, 32 * MB), 42 * MB);
    GC_EXPECT_EQ(Uncommitter::MinCapacity(0, 32 * MB), 32 * MB);
}

GC_TEST(Uncommitter, FlushKeepsMinCapacityAndCapsChunk)
{
    size_t used = 10 * MB;
    size_t dirty = 2 * GB;
    size_t minCap = Uncommitter::MinCapacity(used, 32 * MB);
    size_t chunk = 256 * MB;
    size_t flush = Uncommitter::FlushBytes(used, dirty, minCap, chunk);
    GC_EXPECT_EQ(flush, chunk);
    GC_EXPECT_TRUE(used + dirty - flush >= minCap);
}

GC_TEST(Uncommitter, FlushZeroWhenAlreadyAtFloor)
{
    size_t used = 10 * MB;
    size_t dirty = 20 * MB;
    size_t minCap = Uncommitter::MinCapacity(used, 32 * MB);
    GC_EXPECT_EQ(Uncommitter::FlushBytes(used, dirty, minCap, 256 * MB), 0ULL);
}

GC_TEST(Uncommitter, FlushZeroWhenDisabledChunk)
{
    GC_EXPECT_EQ(Uncommitter::FlushBytes(10 * MB, 2 * GB, 42 * MB, 0), 0ULL);
    GC_EXPECT_EQ(Uncommitter::FlushBytes(10 * MB, 0, 42 * MB, 256 * MB), 0ULL);
}

GC_TEST(Uncommitter, ChunkLimitAtLeastPageAndAtMost256M)
{
    size_t oneG = 1024 * MB;
    size_t chunk = Uncommitter::ChunkLimit(oneG);
    GC_EXPECT_TRUE(chunk >= 4096);
    GC_EXPECT_TRUE(chunk <= Uncommitter::kMaxUncommitChunk);
    GC_EXPECT_EQ(Uncommitter::ChunkLimit(32 * GB), Uncommitter::kMaxUncommitChunk);
}
