// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Semantic port of OpenJDK test_zBitField.cpp onto RegionInfo's product
// BitField<T>.  The product API returns the field in its encoded position, so
// Decode shifts it back before comparing with the original value.

#include <cstdint>

#include "Heap/Allocator/RegionInfo.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

template<typename T>
T Decode(const BitField<T>& field, size_t pos, size_t bits)
{
    return static_cast<T>(field.GetAtomicValue(pos, bits) >> pos);
}

} // namespace

GC_TEST(ZBitFieldPort, EncodeDecodeRoundTrips)
{
    {
        BitField<uint64_t> field{};
        const bool value = false;
        field.SetAtomicValue(0, 1, value);
        GC_EXPECT_EQ(Decode(field, 0, 1), static_cast<uint64_t>(value));
    }
    {
        BitField<uint64_t> field{};
        const bool value = true;
        field.SetAtomicValue(0, 1, value);
        GC_EXPECT_EQ(Decode(field, 0, 1), static_cast<uint64_t>(value));
    }
    {
        BitField<uint64_t> field{};
        const uint8_t value = static_cast<uint8_t>(~uint8_t(0));
        field.SetAtomicValue(1, 8, value);
        GC_EXPECT_EQ(Decode(field, 1, 8), static_cast<uint64_t>(value));
    }
    {
        BitField<uint64_t> field{};
        const uint16_t value = static_cast<uint16_t>(~uint16_t(0));
        field.SetAtomicValue(2, 16, value);
        GC_EXPECT_EQ(Decode(field, 2, 16), static_cast<uint64_t>(value));
    }
    {
        BitField<uint64_t> field{};
        const uint32_t value = ~uint32_t(0);
        field.SetAtomicValue(32, 32, value);
        GC_EXPECT_EQ(Decode(field, 32, 32), static_cast<uint64_t>(value));
    }
    {
        BitField<uint64_t> field{};
        const uint64_t value = ~uint64_t(0) >> 1;
        field.SetAtomicValue(0, 63, value);
        GC_EXPECT_EQ(Decode(field, 0, 63), value);
    }
    {
        BitField<uint64_t> field{};
        const uintptr_t value = ~uintptr_t(0) << 3;
        const uint64_t compressed = static_cast<uint64_t>(value >> 3);
        field.SetAtomicValue(1, 61, compressed);
        const uintptr_t decoded = static_cast<uintptr_t>(Decode(field, 1, 61)) << 3;
        GC_EXPECT_EQ(decoded, value);
    }
}
