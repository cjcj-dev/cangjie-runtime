// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zArray.cpp onto ZArray /
// ZArraySlice / ZArrayIterator, plus the ZActivatedArray / ZSafeDelete
// deferred-delete contract (zSafeDelete.inline.hpp:45-59).

#include <atomic>
#include <cstdlib>
#include <utility>
#include <vector>

#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zSafeDelete.inline.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZArray, sanity)
{
    ZArray<int> a;

    // Add elements
    for (int i = 0; i < 10; i++) {
        a.append(i);
    }

    ZArray<int> b;

    b.swap(&a);

    // Check size
    GC_EXPECT_EQ(a.length(), 0);
    GC_EXPECT_EQ(a.capacity(), 0);
    GC_EXPECT_EQ(a.is_empty(), true);

    GC_EXPECT_EQ(b.length(), 10);
    GC_EXPECT_TRUE(b.capacity() >= 10);
    GC_EXPECT_EQ(b.is_empty(), false);

    // Clear elements
    a.clear();

    // Check that b is unaffected
    GC_EXPECT_EQ(b.length(), 10);
    GC_EXPECT_TRUE(b.capacity() >= 10);
    GC_EXPECT_EQ(b.is_empty(), false);

    a.append(1);

    // Check that b is unaffected
    GC_EXPECT_EQ(b.length(), 10);
    GC_EXPECT_TRUE(b.capacity() >= 10);
    GC_EXPECT_EQ(b.is_empty(), false);
}

GC_TEST(ZArray, iterator)
{
    ZArray<int> a;

    // Add elements
    for (int i = 0; i < 10; i++) {
        a.append(i);
    }

    // Iterate
    int count = 0;
    ZArrayIterator<int> iter(&a);
    for (int value; iter.next(&value);) {
        GC_EXPECT_EQ(a.at(count), count);
        count++;
    }

    // Check count
    GC_EXPECT_EQ(count, 10);
}

namespace {

// ZTest::random() (zunittest.hpp) is the gtest fixture's seeded generator.
long TestRandom()
{
    static unsigned long state = 0x5DEECE66DUL;
    state = state * 6364136223846793005UL + 1442695040888963407UL;
    return static_cast<long>((state >> 33) & 0x7FFFFFFF);
}

template <typename Slice>
void ReverseSlice(Slice slice)
{
    const int length = slice.length();
    if (length > 1) {
        const int middle = length / 2;
        ReverseSlice(slice.slice_front(middle));
        ReverseSlice(slice.slice_back(length - middle));
        auto s1 = slice.slice_front(middle);
        auto s2 = slice.slice_back(length - middle);
        GC_EXPECT_EQ(s1.length(), s2.length());
        for (int i = 0; i < s1.length(); ++i) {
            std::swap(s1.at(i), s2.at(i));
        }
    }
}

void CheckReversed(ZArraySlice<const int> original, ZArraySlice<int> reversed)
{
    GC_EXPECT_EQ(original.length(), reversed.length());
    int ri = reversed.length();
    for (int e : original) {
        GC_EXPECT_EQ(e, reversed.at(--ri));
    }
    GC_EXPECT_EQ(ri, 0);
}

void ReverseTest(const ZArray<int>& original)
{
    ZArray<int> a(original.capacity());
    a.appendAll(&original);

    ReverseSlice(ZArraySlice<int>(a));
    CheckReversed(original, a);
}

int Partition(ZArraySlice<int> slice)
{
    const int p = slice.last();
    int pi = 0;
    for (int i = 0; i < slice.length() - 1; ++i) {
        if (slice.at(i) < p) {
            std::swap(slice.at(i), slice.at(pi++));
        }
    }
    std::swap(slice.at(pi), slice.at(slice.length() - 1));
    return pi;
}

void QuickSort(ZArraySlice<int> slice)
{
    if (slice.length() > 1) {
        const int pi = Partition(slice);
        QuickSort(slice.slice_front(pi));
        QuickSort(slice.slice_back(pi + 1));
    }
}

void VerifySorted(ZArraySlice<const int> slice)
{
    for (int i = 0; i < slice.length(); ++i) {
        int e = slice.at(i);
        for (int l : slice.slice_front(i)) {
            GC_EXPECT_TRUE(e >= l);
        }
        for (int g : slice.slice_back(i)) {
            GC_EXPECT_TRUE(e <= g);
        }
    }
}

void SortTest(const ZArray<int>& original)
{
    ZArray<int> a(original.capacity());
    a.appendAll(&original);

    ZArraySlice<int> slice(a);
    for (int i = 1; i < slice.length(); ++i) {
        const int random_index = static_cast<int>(TestRandom() % (i + 1));
        std::swap(slice.at(i), slice.at(random_index));
    }
    QuickSort(slice);
    VerifySorted(slice);
}

} // namespace

GC_TEST(ZArrayTest, slice)
{
    ZArray<int> a0(0);
    ZArray<int> a10(10);
    ZArray<int> ar(10 + static_cast<int>(std::abs(TestRandom() % 10)));

    // Add elements
    for (int i = 0; i < ar.capacity(); ++i) {
        const auto append = [&](ZArray<int>& a) {
            if (i < a.capacity()) {
                a.append(i);
            }
        };

        append(a0);
        append(a10);
        append(ar);
    }

    ReverseTest(a0);
    ReverseTest(a10);
    ReverseTest(ar);

    SortTest(a0);
    SortTest(a10);
    SortTest(ar);
}

// utilities/growableArray.hpp:399-407 insert_before(idx == length()) is an
// append: the inserted element is the new last element (a shifted copy of
// the old last element is not).
GC_TEST(ZArray, insert_before_at_end_appends)
{
    ZArray<int> a;
    a.insert_before(a.length(), 7);
    GC_EXPECT_EQ(a.length(), 1);
    GC_EXPECT_EQ(a.last(), 7);

    for (int i = 0; i < 3; i++) {
        a.append(i);
    }
    // Target: the element inserted at length() is last().
    a.insert_before(a.length(), 42);
    GC_EXPECT_EQ(a.length(), 5);
    GC_EXPECT_EQ(a.last(), 42);
    GC_EXPECT_EQ(a.at(3), 2);

    // Middle insert keeps order.
    a.insert_before(1, 99);
    GC_EXPECT_EQ(a.length(), 6);
    GC_EXPECT_EQ(a.at(0), 7);
    GC_EXPECT_EQ(a.at(1), 99);
    GC_EXPECT_EQ(a.at(2), 0);
    GC_EXPECT_EQ(a.last(), 42);
}

namespace {

struct Lifetime {
    static std::atomic<int> alive;
    Lifetime() { alive.fetch_add(1); }
    Lifetime(const Lifetime&) { alive.fetch_add(1); }
    Lifetime& operator=(const Lifetime&) = default;
    ~Lifetime() { alive.fetch_sub(1); }
};
std::atomic<int> Lifetime::alive{ 0 };

} // namespace

// utilities/growableArray.hpp:333-346,538-551,587-599 element model: every
// allocated slot is constructed once and destroyed once; append/clear/grow
// assign into constructed slots and never construct over a live element.
GC_TEST(ZArray, elements_are_constructed_and_destroyed_exactly_once)
{
    GC_EXPECT_EQ(Lifetime::alive.load(), 0);
    {
        ZArray<Lifetime> a(2);
        a.append(Lifetime());
        a.append(Lifetime());
        a.clear();
        // Re-append after clear, then grow past the initial capacity.
        for (int i = 0; i < 5; i++) {
            a.append(Lifetime());
        }
        GC_EXPECT_EQ(a.length(), 5);
        // Every live Lifetime is one of the constructed slots.
        GC_EXPECT_EQ(Lifetime::alive.load(), a.capacity());
        (void)a.pop();
        a.trunc_to(2);
        GC_EXPECT_EQ(Lifetime::alive.load(), a.capacity());
    }
    // Target: destruction balances construction.
    GC_EXPECT_EQ(Lifetime::alive.load(), 0);
}

// zArray.inline.hpp:118-190 ZArrayParallelIterator: every index is claimed
// exactly once across concurrent claimers.
GC_TEST(ZArray, parallel_iterator_claims_each_index_once)
{
    constexpr size_t kCount = 4096;
    std::vector<size_t> values(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        values[i] = i;
    }
    std::vector<std::atomic<unsigned>> visits(kCount);
    for (auto& visit : visits) {
        visit.store(0);
    }
    ZArrayParallelIterator<size_t> iter(values.data(), values.size());
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (size_t value; iter.next(&value);) {
                visits[value].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    size_t total = 0;
    for (const auto& visit : visits) {
        GC_EXPECT_EQ(visit.load(), 1u);
        total += visit.load();
    }
    GC_EXPECT_EQ(total, kCount);
}

namespace {

struct DeleteWitness {
    static std::atomic<int> alive;
    DeleteWitness() { alive.fetch_add(1); }
    ~DeleteWitness() { alive.fetch_sub(1); }
};
std::atomic<int> DeleteWitness::alive{ 0 };

} // namespace

// zSafeDelete.inline.hpp:45-59 / zArray.inline.hpp:219-246. Invariant 2:
// objects scheduled while deferred delete is enabled stay alive until the
// last disable, then every one is deleted exactly once; a schedule outside
// the window deletes immediately.
GC_TEST(ZSafeDelete, deferred_delete_waits_for_disable)
{
    ZSafeDelete<DeleteWitness> safe;
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 0);

    // Outside the window: immediate delete.
    safe.schedule_delete(new DeleteWitness());
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 0);

    // Nested enable: nothing is deleted until the outermost disable.
    safe.enable_deferred_delete();
    safe.enable_deferred_delete();
    for (int i = 0; i < 5; ++i) {
        safe.schedule_delete(new DeleteWitness());
    }
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 5);

    safe.disable_deferred_delete();
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 5);

    // The target invariant: the last disable releases all five, exactly once.
    safe.disable_deferred_delete();
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 0);

    // A later window starts empty: nothing from the previous window is
    // deleted twice, and a fresh schedule is again deferred.
    safe.enable_deferred_delete();
    safe.schedule_delete(new DeleteWitness());
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 1);
    safe.disable_deferred_delete();
    GC_EXPECT_EQ(DeleteWitness::alive.load(), 0);
}

// zArray.inline.hpp:192-246 ZActivatedArray with locked = false: the
// unlocked form (used by single-threaded owners) has the same
// activate/add/deactivate contract.
GC_TEST(ZActivatedArray, unlocked_add_if_activated)
{
    ZActivatedArray<int> array(false /* locked */);
    int items[3] = { 1, 2, 3 };
    GC_EXPECT_FALSE(array.is_activated());
    GC_EXPECT_FALSE(array.add_if_activated(&items[0]));

    array.activate();
    GC_EXPECT_TRUE(array.is_activated());
    GC_EXPECT_TRUE(array.add_if_activated(&items[1]));
    GC_EXPECT_TRUE(array.add_if_activated(&items[2]));

    int applied = 0;
    array.deactivate_and_apply([&](int* item) {
        applied += *item;
    });
    GC_EXPECT_EQ(applied, 5);
    GC_EXPECT_FALSE(array.is_activated());
}

// Product wiring: ZPage::RetirePage schedules on the page allocator's
// ZSafeDelete (zPageAllocator.cpp:2248-2250) and the page-owner walk brackets
// safe destroy (zPageTable.cpp:83-98). A page retired during the walk is
// retired exactly once, after the walk ends; outside a walk it is immediate.
GC_TEST(ZSafeDelete, page_retirement_defers_until_page_walk_ends)
{
    GcHeapFixture fx;
    std::atomic<int> retired{ 0 };
    int seenDuringWalk = -1;
    ZPage::VisitPageOwners([&](ZPage* page) {
        if (page == fx.region0()) {
            ZPage::RetirePage(page, [&] { retired.fetch_add(1); });
            seenDuringWalk = retired.load();
        }
    });
    // Precondition: the walk observed the page and nothing ran inside it.
    GC_EXPECT_EQ(seenDuringWalk, 0);
    // Target invariant: the deferred retirement ran once after the walk.
    GC_EXPECT_EQ(retired.load(), 1);

    ZPage::RetirePage(fx.region1(), [&] { retired.fetch_add(1); });
    GC_EXPECT_EQ(retired.load(), 2);
}
