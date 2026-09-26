// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Base/Globals.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ZGC utilities/align.hpp:62-83. These are distinct types on both Linux and
// Apple, even when their widths agree. Test the actual public template.
GC_TEST(Align, mixed_integer_types)
{
    volatile unsigned long long address = 0x123456780009ULL;
    volatile unsigned long alignment = 8UL;
    GC_EXPECT_EQ(AlignUp(address, alignment), 0x123456780010ULL);
    GC_EXPECT_EQ(AlignDown(address, alignment), 0x123456780008ULL);
    GC_EXPECT_EQ(AlignUp(alignment + 1UL, 8ULL), 16UL);
    GC_EXPECT_EQ(AlignDown(alignment + 1UL, 8ULL), 8UL);
    static_assert(std::is_same<decltype(AlignUp(1ULL, 8UL)), unsigned long long>::value, "return value type");
    static_assert(std::is_same<decltype(AlignDown(1UL, 8ULL)), unsigned long>::value, "return value type");
}

GC_TEST(Align, narrow_alignment_preserves_high_bits)
{
    volatile uint64_t address = UINT64_C(0x123456780009);
    volatile uint32_t alignment = 8;
    GC_EXPECT_EQ(AlignUp(address, alignment), UINT64_C(0x123456780010));
    GC_EXPECT_EQ(AlignDown(address, alignment), UINT64_C(0x123456780008));
}

GC_TEST(Align, same_type_boundaries)
{
    volatile size_t alignment = 8;
    GC_EXPECT_EQ(AlignUp(size_t{0}, alignment), size_t{0});
    GC_EXPECT_EQ(AlignUp(size_t{8}, alignment), size_t{8});
    GC_EXPECT_EQ(AlignDown(size_t{8}, alignment), size_t{8});
    GC_EXPECT_EQ(AlignUp(size_t{9}, alignment), size_t{16});
    GC_EXPECT_EQ(AlignDown(size_t{9}, alignment), size_t{8});
}

GC_TEST(Align, explicit_value_type_and_enum_alignment)
{
    enum class Boundary : unsigned { Word = 8 };
    GC_EXPECT_EQ(AlignUp<uint16_t>(uint16_t{9}, size_t{8}), uint16_t{16});
    GC_EXPECT_EQ(AlignDown<uint16_t>(uint16_t{9}, size_t{8}), uint16_t{8});
    GC_EXPECT_EQ(AlignUp(9U, Boundary::Word), 16U);
    GC_EXPECT_EQ(AlignDown(9U, Boundary::Word), 8U);
}

GC_TEST(Align, pointer_delegates_to_integer_alignment)
{
    alignas(64) unsigned char bytes[128] = {};
    volatile unsigned alignment = 64;
    GC_EXPECT_TRUE(AlignUp(bytes + 1, alignment) == bytes + 64);
    GC_EXPECT_TRUE(AlignUp(bytes, alignment) == bytes);
    const unsigned char* ptr = bytes + 1;
    GC_EXPECT_TRUE(AlignUp(ptr, alignment) == bytes + 64);
}

GC_TEST(Align, overflow_precondition)
{
    const auto limit = std::numeric_limits<uint64_t>::max();
    GC_EXPECT_TRUE(CanAlignUp(limit - 7, 8U));
    GC_EXPECT_TRUE(!CanAlignUp(limit - 6, 8U));
    GC_EXPECT_TRUE(CanAlignUp(limit, 1U));
    GC_EXPECT_EQ(AlignUp(limit - 7, 8U), limit - 7);
}
