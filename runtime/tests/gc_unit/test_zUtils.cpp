// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zUtils.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Exercise the public header, including offset calculation and both Copy branches.
// Atomic access itself is checked by test_zUtils_atomic_ir.py with the product recipe.
GC_TEST(ZUtils, atomic_copy_preserves_bounds_and_offset)
{
    uintptr_t source[36];
    uintptr_t target[36];
    for (size_t count = 0; count <= 32; ++count) {
        for (size_t i = 0; i < 36; ++i) {
            source[i] = 0x12340000 + i;
            target[i] = 0xfeed;
        }
        ZUtils::object_copy_disjoint_atomic(static_cast<zaddress>(reinterpret_cast<uintptr_t>(source)),
            static_cast<zaddress>(reinterpret_cast<uintptr_t>(target)), 2 * sizeof(uintptr_t), count * sizeof(uintptr_t));
        for (size_t i = 0; i < 36; ++i) {
            GC_EXPECT_EQ(target[i], (i >= 2 && i < count + 2) ? source[i] : uintptr_t(0xfeed));
            GC_EXPECT_EQ(source[i], uintptr_t(0x12340000 + i));
        }
    }
}

GC_TEST(ZUtils, atomic_copy_adjacent_ranges)
{
    uintptr_t words[34];
    for (size_t i = 0; i < 17; ++i) {
        words[i] = i + 42;
        words[i + 17] = 0;
    }
    ZUtils::object_copy_disjoint_atomic(static_cast<zaddress>(reinterpret_cast<uintptr_t>(words)),
        static_cast<zaddress>(reinterpret_cast<uintptr_t>(words + 17)), 0, 17 * sizeof(uintptr_t));
    for (size_t i = 0; i < 17; ++i) {
        GC_EXPECT_EQ(words[i + 17], words[i]);
    }
    // The reverse address ordering uses the other disjointness branch.
    Copy::disjoint_words_atomic(words + 17, words, 17);
    for (size_t i = 0; i < 17; ++i) {
        GC_EXPECT_EQ(words[i], uintptr_t(i + 42));
    }
    Copy::disjoint_words_atomic(words, words, 0);
}
