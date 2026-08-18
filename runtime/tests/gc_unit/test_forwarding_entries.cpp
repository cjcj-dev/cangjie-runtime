// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of test/hotspot/gtest/gc/z/test_zForwarding.cpp:
// setup / find_empty / find_full / find_every_other.

#include "Heap/Allocator/ForwardingEntry.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct SequenceToFromIndex {
    static uintptr_t even(size_t n) { return n * 2; }
    static uintptr_t odd(size_t n) { return even(n) + 1; }
    static uintptr_t one_to_one(size_t n) { return n; }
};

static bool is_power_of_2(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

static void setup(ForwardingEntries* forwarding)
{
    GC_EXPECT_TRUE(is_power_of_2(forwarding->length()));
}

static void find_empty(ForwardingEntries* forwarding)
{
    const size_t size = forwarding->length();
    const size_t entries_to_check = size * 2;
    for (size_t i = 0; i < entries_to_check; i++) {
        const uintptr_t from_index = SequenceToFromIndex::one_to_one(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_FALSE(entry.populated());
    }
}

static void find_full(ForwardingEntries* forwarding)
{
    const size_t size = forwarding->length();
    const size_t entries_to_populate = size;
    for (size_t i = 0; i < entries_to_populate; i++) {
        const uintptr_t from_index = SequenceToFromIndex::one_to_one(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_FALSE(entry.populated());
        forwarding->insert(from_index, from_index, &cursor);
    }
    for (size_t i = 0; i < entries_to_populate; i++) {
        const uintptr_t from_index = SequenceToFromIndex::one_to_one(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_TRUE(entry.populated());
        GC_EXPECT_EQ(entry.from_index(), from_index);
        GC_EXPECT_EQ(entry.to_offset(), from_index);
    }
}

static void find_every_other(ForwardingEntries* forwarding)
{
    const size_t size = forwarding->length();
    const size_t entries_to_populate = size / 2;
    for (size_t i = 0; i < entries_to_populate; i++) {
        const uintptr_t from_index = SequenceToFromIndex::even(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_FALSE(entry.populated());
        forwarding->insert(from_index, from_index, &cursor);
    }
    for (size_t i = 0; i < entries_to_populate; i++) {
        const uintptr_t from_index = SequenceToFromIndex::even(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_TRUE(entry.populated());
        GC_EXPECT_EQ(entry.from_index(), from_index);
        GC_EXPECT_EQ(entry.to_offset(), from_index);
    }
    for (size_t i = 0; i < entries_to_populate; i++) {
        const uintptr_t from_index = SequenceToFromIndex::odd(i);
        ForwardingCursor cursor = 0;
        const ForwardingEntry entry = forwarding->find(from_index, &cursor);
        GC_EXPECT_FALSE(entry.populated());
    }
}

static void test(void (*function)(ForwardingEntries*), uint32_t size)
{
    ForwardingEntries* forwarding = ForwardingEntries::Create(size, 0, 0);
    GC_EXPECT_TRUE(forwarding != nullptr);
    (*function)(forwarding);
    forwarding->Destroy();
}

static void test(void (*function)(ForwardingEntries*))
{
    test(function, 1);
    test(function, 2);
    test(function, 3);
    test(function, 4);
    test(function, 7);
    test(function, 8);
    test(function, 1023);
    test(function, 1024);
    test(function, 1025);
}

} // namespace

GC_TEST(ZForwardingEntries, setup)
{
    test(&setup);
}

GC_TEST(ZForwardingEntries, find_empty)
{
    test(&find_empty);
}

GC_TEST(ZForwardingEntries, find_full)
{
    test(&find_full);
}

GC_TEST(ZForwardingEntries, find_every_other)
{
    test(&find_every_other);
}

GC_TEST(ZForwardingEntries, survives_without_geometry)
{
    ForwardingEntries* tab = ForwardingEntries::Create(4, 0x1000, 0);
    GC_EXPECT_TRUE(tab != nullptr);
    const MAddress from = 0x1000 + 16;
    const MAddress to = 0x2000 + 32;
    GC_EXPECT_EQ(tab->insert(from, to), to);
    GC_EXPECT_EQ(tab->find(from), to);
    GC_EXPECT_EQ(tab->find(0x1000 + 24), static_cast<MAddress>(0));
    tab->Destroy();
}

// 18 bits of from_index at 8-byte alignment addresses exactly 2 MB, the size of a ZGC small page.
// Our regions are not capped at 2 MB, and the same field wraps quietly on a larger one: two objects
// whose indices differ by 2^18 would compare equal and find() would return the wrong to-address.
// A miss is safe -- FindToVersion falls back to route geometry, which is what it did before this
// table existed -- so an index that does not fit is refused rather than truncated.
GC_TEST(ZForwardingEntries, OutOfRangeFromIndexIsRefusedNotTruncated)
{
    constexpr MAddress kStart = 0x1000;
    constexpr size_t kAlign = size_t(1) << 3;
    ForwardingEntries* tab = ForwardingEntries::Create(8, kStart, 0);
    GC_EXPECT_TRUE(tab != nullptr);

    const MAddress inRange = kStart + (ForwardingEntry::kMaxFromIndex * kAlign);
    const MAddress justOver = kStart + ((ForwardingEntry::kMaxFromIndex + 1) * kAlign);
    const MAddress dest = 0x9000;

    const uint64_t before = ForwardingEntries::OverflowRefusals().load(std::memory_order_relaxed);

    // The last index the field can hold is stored and found.
    GC_EXPECT_EQ(tab->insert(inRange, dest), dest);
    GC_EXPECT_EQ(tab->find(inRange), dest);

    // One past it is refused, and the refusal is counted -- otherwise a truncating build and a
    // guarded one look identical from the outside.
    GC_EXPECT_EQ(tab->insert(justOver, dest + 8), static_cast<MAddress>(0));
    GC_EXPECT_EQ(ForwardingEntries::OverflowRefusals().load(std::memory_order_relaxed), before + 1);

    // The refusal reads as "no entry", not as the entry belonging to the aliasing index. Without
    // the guard justOver truncates onto inRange and find() hands back inRange's destination --
    // a wrong to-address, which is worse than the miss the caller already knows how to handle.
    GC_EXPECT_EQ(tab->find(justOver), static_cast<MAddress>(0));
    GC_EXPECT_EQ(tab->find(inRange), dest);
    tab->Destroy();
}
