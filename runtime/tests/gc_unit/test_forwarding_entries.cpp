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

// ---------------------------------------------------------------------------------------------
// The two defects a real collection found and this suite did not.
//
// Both were merged, passed 120 tests, and then broke the first workload they met. They share a
// shape: each needs more than one region, or more objects than a hand-built table holds, and every
// test above works on a single table sized for the objects it is about to be given. A suite can be
// green and still have never posed the question.

// ZGC's probe loop terminates because ZForwarding sizes _entries to next_pow2(live_objects * 2), so
// the table cannot fill (zForwarding.inline.hpp:43-50). We sized ours from an estimate that could
// come out at one, and the loop is the same loop -- two GC threads sat at 100% CPU inside find()
// while a ten-second workload ran past a five-minute timeout.
//
// Filling the table on purpose is the whole test. A miss on a full table must come back, because a
// miss is a state the caller handles by falling back to route geometry; not coming back is not.
GC_TEST(ZForwardingEntries, FullTableProbeTerminatesInsteadOfSpinning)
{
    constexpr MAddress kStart = 0x1000;
    constexpr size_t kAlign = size_t(1) << 3;
    ForwardingEntries* tab = ForwardingEntries::Create(2, kStart, 0);
    GC_EXPECT_TRUE(tab != nullptr);

    const size_t capacity = tab->length();
    GC_EXPECT_TRUE(capacity > 0);

    // Fill every slot. Distinct from-indices, so nothing collapses onto an existing entry.
    for (size_t i = 0; i < capacity; ++i) {
        const MAddress from = kStart + (i * kAlign);
        GC_EXPECT_TRUE(tab->insert(from, 0x9000 + (i * kAlign)) != 0);
    }

    // A key that is not in a full table: the probe visits every slot, finds no empty one, and has to
    // stop anyway. Before the bound this call did not return.
    const MAddress absent = kStart + (capacity * kAlign);
    GC_EXPECT_EQ(tab->find(absent), static_cast<MAddress>(0));

    // Inserting into a full table is refused rather than retried forever, and the refusal is
    // counted -- "the table declined" and "the table never had it" are different facts.
    const uint64_t before = ForwardingEntries::FullRefusals().load(std::memory_order_relaxed);
    GC_EXPECT_EQ(tab->insert(absent, 0x9000), static_cast<MAddress>(0));
    GC_EXPECT_EQ(ForwardingEntries::FullRefusals().load(std::memory_order_relaxed), before + 1);

    // The entries that were there are still there and still correct.
    GC_EXPECT_EQ(tab->find(kStart), static_cast<MAddress>(0x9000));
    tab->Destroy();
}

// EnsureEntries publishes one table pointer across every unit slot a region covers, so a non-null
// pointer in a slot is not proof of ownership. ClearEntries used to free on that alone, which frees
// a neighbour's live table; the neighbour then frees it again. The crash was a SIGSEGV inside
// __libc_free during post_trace, on the chunk header of an address already returned.
//
// The table records the address it was built for, so this is checkable without a heap: two tables,
// two starts, and the question "does this pointer belong to the region being cleared".
GC_TEST(ZForwardingEntries, TableKnowsWhichRegionItWasBuiltFor)
{
    constexpr MAddress kRegionA = 0x10000;
    constexpr MAddress kRegionB = 0x20000;

    ForwardingEntries* a = ForwardingEntries::Create(4, kRegionA, 0);
    ForwardingEntries* b = ForwardingEntries::Create(4, kRegionB, 0);
    GC_EXPECT_TRUE(a != nullptr && b != nullptr);

    // Ownership is a comparison against a recorded address, not a convention about which slot the
    // pointer was read out of.
    GC_EXPECT_EQ(a->start(), kRegionA);
    GC_EXPECT_EQ(b->start(), kRegionB);
    GC_EXPECT_TRUE(a->start() != b->start());

    // The predicate ClearEntries applies: clearing region B must not free a table built for A, no
    // matter which slot handed it over.
    GC_EXPECT_TRUE(!(a->start() == kRegionB));
    GC_EXPECT_TRUE(b->start() == kRegionB);

    a->Destroy();
    b->Destroy();
}

// e57ae807: reused-region empty-table miss still needs the retired generation.
// covers() is the predicate FindRetiredTo uses to pick the right unlinked table.
GC_TEST(ZForwardingEntries, CoversRecordsRegionSpan)
{
    constexpr MAddress kStart = 0x10000;
    constexpr size_t kSize = 0x1000;
    ForwardingEntries* tab = ForwardingEntries::Create(4, kStart, 0, kSize);
    GC_EXPECT_TRUE(tab != nullptr);
    GC_EXPECT_TRUE(tab->covers(kStart));
    GC_EXPECT_TRUE(tab->covers(kStart + kSize - 8));
    GC_EXPECT_FALSE(tab->covers(kStart + kSize));
    GC_EXPECT_FALSE(tab->covers(kStart - 8));
    ForwardingEntries* bare = ForwardingEntries::Create(4, kStart, 0);
    GC_EXPECT_TRUE(bare != nullptr);
    GC_EXPECT_FALSE(bare->covers(kStart));
    tab->Destroy();
    bare->Destroy();
}
