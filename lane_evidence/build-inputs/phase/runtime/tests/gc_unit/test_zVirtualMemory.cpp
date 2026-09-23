// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of test/hotspot/gtest/gc/z/test_zVirtualMemory.cpp:28-146 (42
// assertion sites). ZGC pins ZAddressOffsetMax to 16T for the max-area case;
// the memory managers here index per ZGranuleSize (one page) rather
// than per ZGranuleSize, so granule_count() fits an int only up to 2^42 bytes.

#include "gc_unittest.hpp"
#include "zunittest.hpp"

using namespace MapleRuntime;

namespace {
constexpr size_t kOffsetMaxForTest = size_t(1) << 42;
}

GC_TEST(ZVirtualMemory, is_null)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    ZVirtualMemory mem;
    GC_EXPECT_TRUE(mem.is_null());
}

GC_TEST(ZVirtualMemory, accessors)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    {
        ZVirtualMemory mem(zoffset(0), ZGranuleSize);

        GC_EXPECT_EQ(mem.start(), zoffset(0));
        GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize));
        GC_EXPECT_EQ(mem.size(), ZGranuleSize);
        GC_EXPECT_EQ(mem.granule_count(), static_cast<int>(ZGranuleSize / ZGranuleSize));
    }

    {
        ZVirtualMemory mem(zoffset(ZGranuleSize), ZGranuleSize);

        GC_EXPECT_EQ(mem.start(), zoffset(ZGranuleSize));
        GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize + ZGranuleSize));
        GC_EXPECT_EQ(mem.size(), ZGranuleSize);
        GC_EXPECT_EQ(mem.granule_count(), static_cast<int>(ZGranuleSize / ZGranuleSize));
    }

    {
        // Max area - check end boundary
        ZVirtualMemory mem(zoffset(0), ZAddressOffsetMax);

        GC_EXPECT_EQ(mem.start(), zoffset(0));
        GC_EXPECT_EQ(mem.end(), zoffset_end(ZAddressOffsetMax));
        GC_EXPECT_EQ(mem.size(), ZAddressOffsetMax);
        GC_EXPECT_EQ(mem.granule_count(), static_cast<int>(ZAddressOffsetMax / ZGranuleSize));
    }
}

GC_TEST(ZVirtualMemory, resize)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    ZVirtualMemory mem(zoffset(ZGranuleSize * 2), ZGranuleSize * 2) ;

    mem.shrink_from_front(ZGranuleSize);
    GC_EXPECT_EQ(mem.start(),   zoffset(ZGranuleSize * 3));
    GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize * 4));
    GC_EXPECT_EQ(mem.size(),            ZGranuleSize * 1);
    mem.grow_from_front(ZGranuleSize);

    mem.shrink_from_back(ZGranuleSize);
    GC_EXPECT_EQ(mem.start(),   zoffset(ZGranuleSize * 2));
    GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize * 3));
    GC_EXPECT_EQ(mem.size(),            ZGranuleSize * 1);
    mem.grow_from_back(ZGranuleSize);

    mem.grow_from_front(ZGranuleSize);
    GC_EXPECT_EQ(mem.start(),   zoffset(ZGranuleSize * 1));
    GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize * 4));
    GC_EXPECT_EQ(mem.size(),            ZGranuleSize * 3);
    mem.shrink_from_front(ZGranuleSize);

    mem.grow_from_back(ZGranuleSize);
    GC_EXPECT_EQ(mem.start(),   zoffset(ZGranuleSize * 2));
    GC_EXPECT_EQ(mem.end(), zoffset_end(ZGranuleSize * 5));
    GC_EXPECT_EQ(mem.size(),            ZGranuleSize * 3);
    mem.shrink_from_back(ZGranuleSize);
}

GC_TEST(ZVirtualMemory, shrink_from_front)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    ZVirtualMemory mem(zoffset(0), ZGranuleSize * 10);

    ZVirtualMemory mem0 = mem.shrink_from_front(0);
    GC_EXPECT_EQ(mem0.size(), 0u);
    GC_EXPECT_EQ(mem.size(), ZGranuleSize * 10);

    ZVirtualMemory mem1 = mem.shrink_from_front(ZGranuleSize * 5);
    GC_EXPECT_EQ(mem1.size(), ZGranuleSize * 5);
    GC_EXPECT_EQ(mem.size(), ZGranuleSize * 5);

    ZVirtualMemory mem2 = mem.shrink_from_front(ZGranuleSize * 5);
    GC_EXPECT_EQ(mem2.size(), ZGranuleSize * 5);
    GC_EXPECT_EQ(mem.size(), 0u);

    ZVirtualMemory mem3 = mem.shrink_from_front(0);
    GC_EXPECT_EQ(mem3.size(), 0u);
}

GC_TEST(ZVirtualMemory, shrink_from_back)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    ZVirtualMemory mem(zoffset(0), ZGranuleSize * 10);

    ZVirtualMemory mem1 = mem.shrink_from_back(ZGranuleSize * 5);
    GC_EXPECT_EQ(mem1.size(), ZGranuleSize * 5);
    GC_EXPECT_EQ(mem.size(), ZGranuleSize * 5);

    ZVirtualMemory mem2 = mem.shrink_from_back(ZGranuleSize * 5);
    GC_EXPECT_EQ(mem2.size(), ZGranuleSize * 5);
    GC_EXPECT_EQ(mem.size(), 0u);
}

GC_TEST(ZVirtualMemory, adjacent_to)
{
    EnsureZAddressDomain();
    ZAddressOffsetMaxSetter setter(kOffsetMaxForTest);

    ZVirtualMemory mem0(zoffset(0), ZGranuleSize);
    ZVirtualMemory mem1(zoffset(ZGranuleSize), ZGranuleSize);
    ZVirtualMemory mem2(zoffset(ZGranuleSize * 2), ZGranuleSize);

    GC_EXPECT_TRUE(mem0.adjacent_to(mem1));
    GC_EXPECT_TRUE(mem1.adjacent_to(mem0));
    GC_EXPECT_TRUE(mem1.adjacent_to(mem2));
    GC_EXPECT_TRUE(mem2.adjacent_to(mem1));

    GC_EXPECT_FALSE(mem0.adjacent_to(mem2));
    GC_EXPECT_FALSE(mem2.adjacent_to(mem0));
}
