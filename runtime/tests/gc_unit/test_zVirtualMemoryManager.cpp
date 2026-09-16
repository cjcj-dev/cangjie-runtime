// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of test/hotspot/gtest/gc/z/test_zVirtualMemoryManager.cpp:37-271 (17
// assertion sites, four cases). The fifth case, insert_merges_neighbours,
// pins the move_into merge branches (zRangeRegistry.inline.hpp:49-63) that the
// upstream cases only exercise indirectly.

#include "gc_unittest.hpp"
#include "zunittest.hpp"
#include "Heap/z/zHeap.hpp"

#include <sys/mman.h>

namespace MapleRuntime {

#define ASSERT_REMOVAL_OK(range, sz) GC_EXPECT_FALSE(range.is_null()); GC_EXPECT_EQ(range.size(), (sz))

class ZVirtualMemoryManagerTest {
private:
  static constexpr size_t ReservationSize = 32 * MB;

  ZTest::ZAddressReserver _zaddress_reserver;
  ZVirtualMemoryReserver* _reserver;
  ZVirtualMemoryRegistry* _registry;
  bool _ready;

public:
  ZVirtualMemoryManagerTest()
    : _reserver(nullptr),
      _registry(nullptr),
      _ready(false) {
    SetUp();
  }

  ~ZVirtualMemoryManagerTest() {
    TearDown();
  }

  bool ready() const { return _ready; }

  void SetUp() {
    _zaddress_reserver.SetUp(ReservationSize);
    _reserver = _zaddress_reserver.reserver();
    _registry = _zaddress_reserver.registry();

    // The upstream fixture skips when the reservation is short or split.
    _ready = _reserver->reserved() >= ReservationSize && _registry->is_contiguous();
  }

  void TearDown() {
    _registry = nullptr;
    _reserver = nullptr;
    _zaddress_reserver.TearDown();
  }

  void test_reserve_discontiguous_and_coalesce() {
    // Start by ensuring that we have 3 unreserved granules, and then let the
    // fourth granule be pre-reserved and therefore blocking subsequent requests
    // to reserve memory.
    //
    // +----+----+----+----+
    //                -----  pre-reserved - to block contiguous reservation
    // ---------------       unreserved   - to allow reservation of 3 granules
    //
    // If we then asks for 4 granules starting at the first granule above,
    // then we won't be able to reserve 4 consecutive granules and the code
    // reverts into the discontiguous mode. This mode uses interval halving
    // to find the limits of memory areas that have already been reserved.
    // This will lead to the first 2 granules being reserved, then the third
    // granule will be reserved.

    // Start at the offset we reserved.
    const zoffset base_offset = _registry->peek_low_address();

    // Empty the reserved memory in preparation for the rest of the test.
    _reserver->unreserve_all();

    const zaddress_unsafe base = ZOffset::address_unsafe(base_offset);
    const uintptr_t blocked = untype(base) + 3 * ZGranuleSize;

    // Reserve the memory that is acting as a blocking reservation.
    {
      void* const result = mmap(reinterpret_cast<void*>(blocked), ZGranuleSize, PROT_NONE,
                                MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
      GC_EXPECT_TRUE(result != MAP_FAILED && reinterpret_cast<uintptr_t>(result) == blocked);
    }

    {
      // This ends up reserving 2 granules and then 1 granule adjacent to the
      // first. The manager is designed to have one coalesced range per memory
      // area, so a subsequent remove_from_low must see one 3 granule range.
      const size_t reserved = _reserver->reserve_discontiguous(base_offset, 4 * ZGranuleSize, ZGranuleSize);
      GC_EXPECT_TRUE(reserved <= 3 * ZGranuleSize);
      GC_EXPECT_EQ(reserved, 3 * ZGranuleSize);
    }

    {
      // The test used to crash here because the 3 granule memory area was
      // inadvertently covered by two place holders (2 granules + 1 granule).
      const ZVirtualMemory vmem = _registry->remove_from_low(2 * ZGranuleSize);
      GC_EXPECT_TRUE(vmem == ZVirtualMemory(base_offset, 2 * ZGranuleSize));

      // Cleanup - Must happen in granule-sizes because of how Windows hands
      // out memory in granule-sized placeholder reservations.
      _reserver->unreserve(vmem.first_part(ZGranuleSize));
      _reserver->unreserve(vmem.last_part(ZGranuleSize));
    }

    // Final cleanup
    const ZVirtualMemory vmem = _registry->remove_from_low(ZGranuleSize);
    GC_EXPECT_TRUE(vmem == ZVirtualMemory(base_offset + 2 * ZGranuleSize, ZGranuleSize));
    _reserver->unreserve(vmem);

    (void)munmap(reinterpret_cast<void*>(blocked), ZGranuleSize);
  }

  void test_remove_from_low() {
    {
      // Verify that we get a placeholder for the first granule
      const ZVirtualMemory removed = _registry->remove_from_low(ZGranuleSize);
      ASSERT_REMOVAL_OK(removed, ZGranuleSize);

      _registry->insert(removed);
    }

    {
      // Remove something larger than a granule and then insert it
      const ZVirtualMemory removed = _registry->remove_from_low(3 * ZGranuleSize);
      ASSERT_REMOVAL_OK(removed, 3 * ZGranuleSize);

      _registry->insert(removed);
    }

    {
      // Insert with more memory removed
      const ZVirtualMemory removed = _registry->remove_from_low(ZGranuleSize);
      ASSERT_REMOVAL_OK(removed, ZGranuleSize);

      ZVirtualMemory next = _registry->remove_from_low(ZGranuleSize);
      ASSERT_REMOVAL_OK(next, ZGranuleSize);

      _registry->insert(removed);
      _registry->insert(next);
    }
  }

  void test_remove_from_high() {
    {
      // Verify that we get a placeholder for the last granule
      const ZVirtualMemory high = _registry->remove_from_high(ZGranuleSize);
      ASSERT_REMOVAL_OK(high, ZGranuleSize);

      const ZVirtualMemory prev = _registry->remove_from_high(ZGranuleSize);
      ASSERT_REMOVAL_OK(prev, ZGranuleSize);

      _registry->insert(high);
      _registry->insert(prev);
    }

    {
      // Remove something larger than a granule and return it
      const ZVirtualMemory high = _registry->remove_from_high(2 * ZGranuleSize);
      ASSERT_REMOVAL_OK(high, 2 * ZGranuleSize);

      _registry->insert(high);
    }
  }

  void test_remove_whole() {
    // Need a local variable to appease gtest
    const size_t reservation_size = ReservationSize;

    // Remove the whole reservation
    const ZVirtualMemory reserved = _registry->remove_from_low(reservation_size);
    ASSERT_REMOVAL_OK(reserved, reservation_size);

    const ZVirtualMemory first(reserved.start(), 4 * ZGranuleSize);
    const ZVirtualMemory second(reserved.start() + 6 * ZGranuleSize, 6 * ZGranuleSize);

    // Insert two chunks and then remove them again
    _registry->insert(first);
    _registry->insert(second);

    const ZVirtualMemory removed_first = _registry->remove_from_low(first.size());
    GC_EXPECT_TRUE(removed_first == first);

    const ZVirtualMemory removed_second = _registry->remove_from_low(second.size());
    GC_EXPECT_TRUE(removed_second == second);

    // Now insert it all, and verify it can be re-removed
    _registry->insert(reserved);

    const ZVirtualMemory removed_reserved = _registry->remove_from_low(reservation_size);
    GC_EXPECT_TRUE(removed_reserved == reserved);

    _registry->insert(reserved);
  }

  // move_into (zRangeRegistry.inline.hpp:35-85): a range handed back between
  // two free neighbours merges with both, one handed back before a free
  // neighbour merges with it, one handed back after merges with prev. The
  // registry must hand the whole run out again as one range.
  void test_insert_merges_neighbours() {
    const ZVirtualMemory a = _registry->remove_from_low(ZGranuleSize);
    const ZVirtualMemory b = _registry->remove_from_low(ZGranuleSize);
    const ZVirtualMemory c = _registry->remove_from_low(ZGranuleSize);
    ASSERT_REMOVAL_OK(a, ZGranuleSize);
    ASSERT_REMOVAL_OK(b, ZGranuleSize);
    ASSERT_REMOVAL_OK(c, ZGranuleSize);
    GC_EXPECT_TRUE(a.adjacent_to(b) && b.adjacent_to(c));

    // Hand back a first: it is not adjacent to the remaining free range, so
    // the registry now holds two ranges.
    _registry->insert(a);
    GC_EXPECT_FALSE(_registry->is_contiguous());

    // c is adjacent to the remaining free range (merge with current), a is
    // still separate.
    _registry->insert(c);
    GC_EXPECT_FALSE(_registry->is_contiguous());

    // b closes the gap: merge with prev (a) and with current (c...).
    _registry->insert(b);
    GC_EXPECT_TRUE(_registry->is_contiguous());

    // Whole run comes back as one range starting at a.
    const ZVirtualMemory run = _registry->remove_from_low(3 * ZGranuleSize);
    ASSERT_REMOVAL_OK(run, 3 * ZGranuleSize);
    GC_EXPECT_TRUE(run.start() == a.start());
    _registry->insert(run);

    // Merge with prev alone: hand out two, hand the second one back first,
    // then the first one; prev-merge must join them into one range.
    const ZVirtualMemory x = _registry->remove_from_low(ZGranuleSize);
    const ZVirtualMemory y = _registry->remove_from_low(ZGranuleSize);
    const ZVirtualMemory z = _registry->remove_from_low(ZGranuleSize);
    ASSERT_REMOVAL_OK(z, ZGranuleSize);
    _registry->insert(x);
    // y sits between x (prev, free) and z (still handed out): merge with prev only.
    _registry->insert(y);
    const ZVirtualMemory xy = _registry->remove_from_low(2 * ZGranuleSize);
    ASSERT_REMOVAL_OK(xy, 2 * ZGranuleSize);
    GC_EXPECT_TRUE(xy.start() == x.start());
    _registry->insert(xy);
    _registry->insert(z);
    GC_EXPECT_TRUE(_registry->is_contiguous());
  }
};

} // namespace MapleRuntime

using namespace MapleRuntime;

#define ZVMM_TEST(name)                                                            \
GC_TEST(ZVirtualMemoryManagerTest, name)                                           \
{                                                                                  \
    ZVirtualMemoryManagerTest fixture;                                             \
    GC_EXPECT_TRUE(fixture.ready());                                               \
    fixture.name();                                                                \
}

ZVMM_TEST(test_reserve_discontiguous_and_coalesce)
ZVMM_TEST(test_remove_from_low)
ZVMM_TEST(test_remove_from_high)
ZVMM_TEST(test_remove_whole)
ZVMM_TEST(test_insert_merges_neighbours)

// P01 reverse-metadata adapter over P04's real reservation producer. Keep the
// holes occupied so both the contiguous search and recursive fallback run.
GC_OTHER_VM_TEST(ZVirtualMemoryManagerTest, InitialSegmentsPreserveNineReservations)
{
    EnsureZAddressDomain();
    constexpr size_t domainSize = 512 * MB;
    constexpr size_t segmentSize = 8 * MB;
    constexpr size_t count = 9;
    const uintptr_t domain = ZAddressHeapBase;
    ZAddressOffsetMaxSetter domainLimit(domainSize);
    void* occupied = mmap(reinterpret_cast<void*>(domain), domainSize, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    GC_EXPECT_TRUE(occupied != MAP_FAILED);
    for (size_t i = 0; i < count; ++i) {
        GC_EXPECT_EQ(munmap(reinterpret_cast<void*>(domain + 2 * i * segmentSize), segmentSize), 0);
    }
    {
        ZVirtualMemoryManager manager(8 * MB);
        GC_EXPECT_TRUE(manager.is_initialized());
        const auto first = RegionManager::ReservedSegments(manager);
        const auto second = RegionManager::ReservedSegments(manager);
        std::fprintf(stderr, "P04_SEGMENTS_TARGET first=%zu second=%zu expected=%zu\n",
                     first.size(), second.size(), count);
        GC_EXPECT_EQ(first.size(), count);
        GC_EXPECT_EQ(second.size(), first.size());
        size_t bytes = 0;
        for (size_t i = 0; i < count; ++i) {
            GC_EXPECT_EQ(first[i].start, domain + 2 * i * segmentSize);
            GC_EXPECT_EQ(first[i].size, segmentSize);
            GC_EXPECT_EQ(second[i].start, first[i].start);
            GC_EXPECT_EQ(second[i].size, first[i].size);
            bytes += first[i].size;
        }
        // A real claim sees exactly the same addresses after both borrow/return
        // operations, including the ninth segment; no interval crosses a hole.
        ZArray<ZVirtualMemory> claimed;
        GC_EXPECT_EQ(manager.remove_from_low_many_at_most(ZAddressOffsetMax, ZPerNUMAStorage::id(), &claimed), bytes);
        GC_EXPECT_EQ(claimed.length(), static_cast<int>(count));
        for (int i = 0; i < claimed.length(); ++i) {
            GC_EXPECT_EQ(untype(ZOffset::address_unsafe(claimed.at(i).start())), first[i].start);
            GC_EXPECT_EQ(claimed.at(i).size(), first[i].size);
            manager.insert(claimed.at(i), ZPerNUMAStorage::id());
        }
    }
    GC_EXPECT_EQ(munmap(reinterpret_cast<void*>(domain), domainSize), 0);
}

// Unit coverage of the header-only P01 provider. The joint llc runner separately
// consumes ranges emitted by RegionSpace::Init from real OS reservations.
GC_OTHER_VM_TEST(ZVirtualMemoryManagerTest, CompilerTablePublishesEveryRange)
{
    EnsureZAddressDomain();
    const uintptr_t domain = ZAddressHeapBase;
    const size_t granule = ZBackingGranuleSize;
    for (size_t count : {size_t(0), size_t(1), size_t(9), size_t(kCjHeapRangeCap)}) {
        std::vector<HeapSlotAddressRange> ranges;
        for (size_t i = 0; i < count; ++i) {
            ranges.push_back({domain + 2 * i * granule, domain + (2 * i + 1) * granule});
        }
        Heap::OnHeapCreated(domain, ranges);
        std::fprintf(stderr, "P04_RANGE_PUBLICATION_TARGET input=%zu published=%zu\n",
                     count, static_cast<size_t>(g_cjHeapRangeCount));
        GC_EXPECT_EQ(g_cjHeapRangeCount, count);
        for (size_t i = 0; i < count; ++i) {
            GC_EXPECT_EQ(g_cjHeapRangeStart[i], ranges[i].start);
            GC_EXPECT_EQ(g_cjHeapRangeEnd[i], ranges[i].end);
            GC_EXPECT_TRUE(Heap::IsHeapAddress(reinterpret_cast<void*>(ranges[i].start)));
            GC_EXPECT_TRUE(Heap::IsHeapAddress(reinterpret_cast<void*>(ranges[i].end - 1)));
            GC_EXPECT_FALSE(Heap::IsHeapAddress(reinterpret_cast<void*>(ranges[i].end)));
        }
        for (size_t i = count; i < kCjHeapRangeCap; ++i) {
            GC_EXPECT_EQ(g_cjHeapRangeStart[i], 0U);
            GC_EXPECT_EQ(g_cjHeapRangeEnd[i], 0U);
        }
    }
}

GC_OTHER_VM_TEST(ZVirtualMemoryManagerTest, CompilerTableRejectsOversizedPublication)
{
    EnsureZAddressDomain();
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        std::vector<HeapSlotAddressRange> ranges;
        for (size_t i = 0; i <= kCjHeapRangeCap; ++i) {
            ranges.push_back({ZAddressHeapBase + 2 * i * ZBackingGranuleSize,
                              ZAddressHeapBase + (2 * i + 1) * ZBackingGranuleSize});
        }
        Heap::OnHeapCreated(ZAddressHeapBase, ranges);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "P04_RANGE_LIMIT_TARGET status=%d\n", status);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}
