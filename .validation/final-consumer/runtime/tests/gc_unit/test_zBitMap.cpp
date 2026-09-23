// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zBitMap.cpp onto
// ZBitMap::par_set_bit_pair (zBitMap.inline.hpp:55-92).

#include <cstddef>

#include "Heap/z/zBitMap.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class ZBitMapTest {
public:
    static void test_set_pair_unset(size_t size, bool finalizable)
    {
        ZBitMap bitmap(size);

        for (BitMap::idx_t i = 0; i < size - 1; i++) {
            if ((i + 1) % BitMap::BitsPerWord == 0) {
                // Can't set pairs of bits in different words.
                continue;
            }

            // ZBitMaps are not cleared when constructed.
            bitmap.clear();

            bool inc_live = false;

            bool ret = bitmap.par_set_bit_pair(i, finalizable, inc_live);
            GC_EXPECT_TRUE(ret);      // Failed to set bit
            GC_EXPECT_TRUE(inc_live); // Should have set inc_live

            // First bit should always be set
            GC_EXPECT_TRUE(bitmap.at(i));

            // Second bit should only be set when marking strong
            GC_EXPECT_NE(bitmap.at(i + 1), finalizable);
        }
    }

    static void test_set_pair_set(size_t size, bool finalizable)
    {
        ZBitMap bitmap(size);

        for (BitMap::idx_t i = 0; i < size - 1; i++) {
            if ((i + 1) % BitMap::BitsPerWord == 0) {
                // Can't set pairs of bits in different words.
                continue;
            }

            // Fill the bitmap with ones.
            bitmap.set_range(0, size);

            bool inc_live = false;

            bool ret = bitmap.par_set_bit_pair(i, finalizable, inc_live);
            GC_EXPECT_FALSE(ret);      // Should not succeed setting bit
            GC_EXPECT_FALSE(inc_live); // Should not have set inc_live

            // Both bits were pre-set.
            GC_EXPECT_TRUE(bitmap.at(i));
            GC_EXPECT_TRUE(bitmap.at(i + 1));
        }
    }

    static void test_set_pair_set(bool finalizable)
    {
        test_set_pair_set(2,   finalizable);
        test_set_pair_set(62,  finalizable);
        test_set_pair_set(64,  finalizable);
        test_set_pair_set(66,  finalizable);
        test_set_pair_set(126, finalizable);
        test_set_pair_set(128, finalizable);
    }

    static void test_set_pair_unset(bool finalizable)
    {
        test_set_pair_unset(2,   finalizable);
        test_set_pair_unset(62,  finalizable);
        test_set_pair_unset(64,  finalizable);
        test_set_pair_unset(66,  finalizable);
        test_set_pair_unset(126, finalizable);
        test_set_pair_unset(128, finalizable);
    }
};

} // namespace

GC_TEST(ZBitMapTest, test_set_pair_set)
{
    ZBitMapTest::test_set_pair_set(false);
    ZBitMapTest::test_set_pair_set(true);
}

GC_TEST(ZBitMapTest, test_set_pair_unset)
{
    ZBitMapTest::test_set_pair_unset(false);
    ZBitMapTest::test_set_pair_unset(true);
}

// Strong marking of a pair whose live bit was already set by a finalizable
// mark succeeds without a second live claim (zBitMap.inline.hpp:74-78:
// inc_live = !(old_val & marked_mask)).
GC_TEST(ZBitMapTest, finalizable_then_strong_claims_live_once)
{
    ZBitMap bitmap(128);
    bitmap.clear();
    bool inc_live = false;
    GC_EXPECT_TRUE(bitmap.par_set_bit_pair(10, true /* finalizable */, inc_live));
    GC_EXPECT_TRUE(inc_live);
    GC_EXPECT_TRUE(bitmap.at(10));
    GC_EXPECT_FALSE(bitmap.at(11));

    inc_live = true;
    GC_EXPECT_TRUE(bitmap.par_set_bit_pair(10, false /* strong */, inc_live));
    GC_EXPECT_FALSE(inc_live);
    GC_EXPECT_TRUE(bitmap.at(10));
    GC_EXPECT_TRUE(bitmap.at(11));

    // A finalizable mark after the strong one changes nothing.
    inc_live = true;
    GC_EXPECT_FALSE(bitmap.par_set_bit_pair(10, true, inc_live));
    GC_EXPECT_FALSE(inc_live);
}

// zBitMap.inline.hpp:93-122 ReverseIterator walks set bits from the end.
GC_TEST(ZBitMapTest, reverse_iterator)
{
    ZBitMap bitmap(200);
    bitmap.clear();
    bitmap.set_bit(3);
    bitmap.set_bit(64);
    bitmap.set_bit(150);
    bitmap.set_bit(199);

    ZBitMap::ReverseIterator iter(&bitmap);
    BitMap::idx_t index = 0;
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 199u);
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 150u);
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 64u);
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 3u);
    GC_EXPECT_FALSE(iter.next(&index));

    iter.reset(100, 200);
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 199u);
    GC_EXPECT_TRUE(iter.next(&index));
    GC_EXPECT_EQ(index, 150u);
    GC_EXPECT_FALSE(iter.next(&index));
}
