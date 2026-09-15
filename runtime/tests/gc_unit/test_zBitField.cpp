// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zBitField.cpp onto the
// ZBitField<Container, Value, Shift, Bits, ValueShift> static encode/decode
// template (zBitField.hpp:59-79).

#include <cstdint>

#include "Heap/z/zBitField.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZBitFieldTest, test)
{
    typedef ZBitField<uint64_t, bool,      0,  1>    field_bool;
    typedef ZBitField<uint64_t, uint8_t,   1,  8>    field_uint8;
    typedef ZBitField<uint64_t, uint16_t,  2, 16>    field_uint16;
    typedef ZBitField<uint64_t, uint32_t, 32, 32>    field_uint32;
    typedef ZBitField<uint64_t, uint64_t,  0, 63>    field_uint64;
    typedef ZBitField<uint64_t, void*,     1, 61, 3> field_pointer;

    uint64_t entry;

    {
        const bool value = false;
        entry = field_bool::encode(value);
        GC_EXPECT_EQ(field_bool::decode(entry), value);
    }

    {
        const bool value = true;
        entry = field_bool::encode(value);
        GC_EXPECT_EQ(field_bool::decode(entry), value);
    }

    {
        const uint8_t value = ~(uint8_t)0;
        entry = field_uint8::encode(value);
        GC_EXPECT_EQ(field_uint8::decode(entry), value);
    }

    {
        const uint16_t value = ~(uint16_t)0;
        entry = field_uint16::encode(value);
        GC_EXPECT_EQ(field_uint16::decode(entry), value);
    }

    {
        const uint32_t value = ~(uint32_t)0;
        entry = field_uint32::encode(value);
        GC_EXPECT_EQ(field_uint32::decode(entry), value);
    }

    {
        const uint64_t value = ~(uint64_t)0 >> 1;
        entry = field_uint64::encode(value);
        GC_EXPECT_EQ(field_uint64::decode(entry), value);
    }

    {
        void* const value = (void*)(~(uintptr_t)0 << 3);
        entry = field_pointer::encode(value);
        GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(field_pointer::decode(entry)), reinterpret_cast<uintptr_t>(value));
    }
}
