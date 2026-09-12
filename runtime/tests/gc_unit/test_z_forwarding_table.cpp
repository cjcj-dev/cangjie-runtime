// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// PORT_ZFORWARDING step ①: ZForwardingTable granule map of ZForwarding* +
// ZForwarding attached-array / refcount skeleton.
// Anchors: zForwardingTable.hpp:32-52, zForwardingTable.inline.hpp:43-62,
//          zForwarding.hpp:44-110, zAttachedArray.inline.hpp:32-84.
//
// The process-global ForwardingTable::Initialize is one-shot (RegionManager.cpp:970
// and test_forwarding_no_geometry). A second Initialize with a different heap is a
// no-op, so these tests exercise ZGranuleMap locally and ZForwarding without
// rebinding the product map.

#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/ZAttachedArray.h"
#include "Heap/Allocator/ZGranuleMap.h"
#include "Heap/Collector/ZForwarding.h"
#include "gc_unittest.hpp"
#include "gc_heap_fixture.hpp"

#include <type_traits>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

static_assert(!std::is_convertible<MAddress, zoffset>::value);
static_assert(!std::is_convertible<zoffset, MAddress>::value);
static_assert(!std::is_convertible<zpointer, zoffset>::value);
static_assert(!std::is_convertible<zaddress, zoffset>::value);
static_assert(!std::is_convertible<zaddress_unsafe, zoffset>::value);

GC_TEST(ZGranuleMap, GetPutRemove)
{
    constexpr MAddress kStart = 0x40000000;
    constexpr size_t kSize = 0x1000;
    ZGranuleMap<ZForwarding*> map;
    GC_EXPECT_TRUE(map.Initialize(kStart, 4 * kSize, kSize));

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    zoffset start;
    zoffset interior;
    zoffset next;
    GC_EXPECT_TRUE(map.offset_for_address(kStart, &start));
    GC_EXPECT_TRUE(map.offset_for_address(kStart + 8, &interior));
    GC_EXPECT_TRUE(map.offset_for_address(kStart + kSize, &next));
    map.put(start, kSize, fwd);
    GC_EXPECT_TRUE(map.get(start) == fwd);
    GC_EXPECT_TRUE(map.get(interior) == fwd);
    GC_EXPECT_TRUE(map.get(next) == nullptr);

    map.put(start, kSize, nullptr);
    GC_EXPECT_TRUE(map.get(start) == nullptr);
    fwd->Destroy();
}

GC_TEST(ZGranuleMap, OffsetBoundaryRejectsBeforeIndex)
{
    constexpr MAddress kStart = 0x41000000;
    constexpr size_t kGranule = 0x1000;
    constexpr size_t kHeapSize = 4 * kGranule;
    ZGranuleMap<ZForwarding*> map;
    GC_EXPECT_TRUE(map.Initialize(kStart, kHeapSize, kGranule));

    zoffset offset = zoffset::invalid;
    GC_EXPECT_TRUE(map.offset_for_address(kStart, &offset));
    GC_EXPECT_EQ(raw(offset), static_cast<Uptr>(0));
    GC_EXPECT_TRUE(map.offset_for_address(kStart + kHeapSize - 1, &offset));
    GC_EXPECT_EQ(raw(offset), static_cast<Uptr>(kHeapSize - 1));
    GC_EXPECT_FALSE(map.offset_for_address(kStart - 1, &offset));
    GC_EXPECT_FALSE(map.offset_for_address(kStart + kHeapSize, &offset));
}

// ZGranuleMap::put/get (zGranuleMap.inline.hpp:62-84) and the highest-offset
// map extent in zPageTable.cpp:37-47. Reserved capacity excludes the hole;
// address indices include it, so publishing either segment cannot alias it.
GC_TEST(ZGranuleMap, DiscontiguousPagesLeaveHoleUnmapped)
{
    constexpr MAddress base = 0x42000000;
    constexpr size_t granule = 0x1000;
    ZGranuleMap<int*> map;
    int first = 1;
    int second = 2;
    GC_EXPECT_TRUE(map.Initialize(base, 5 * granule, granule));
    map.put(static_cast<zoffset>(0), 2 * granule, &first);
    map.put(static_cast<zoffset>(3 * granule), 2 * granule, &second);
    for (size_t i = 0; i < 5; ++i) {
        zoffset offset;
        GC_EXPECT_TRUE(map.offset_for_address(base + i * granule + granule - 1, &offset));
        GC_EXPECT_TRUE(map.get(offset) == (i < 2 ? &first : i == 2 ? nullptr : &second));
    }
    map.put(static_cast<zoffset>(0), 2 * granule, nullptr);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(0)) == nullptr);
    GC_EXPECT_TRUE(map.get(static_cast<zoffset>(3 * granule)) == &second);
}

GC_TEST(ZGranuleMap, AddressExtentPreservesLow48Budget)
{
    constexpr size_t granule = 0x1000;
    ZGranuleMap<int*> map;
    zoffset offset = zoffset::invalid;
    GC_EXPECT_FALSE(map.offset_for_address(0, &offset));
    GC_EXPECT_FALSE(map.Initialize(kPointerAddressLimit - granule, 2 * granule, granule));
    GC_EXPECT_FALSE(map.Initialize(kPointerAddressLimit, granule, granule));
    GC_EXPECT_FALSE(map.Initialize(0x1000, granule + 1, granule));
    GC_EXPECT_TRUE(map.Initialize(kPointerAddressLimit - granule, granule, granule));
    GC_EXPECT_TRUE(map.offset_for_address(kPointerAddressLimit - 1, &offset));
    GC_EXPECT_EQ(raw(offset), static_cast<Uptr>(granule - 1));
    GC_EXPECT_FALSE(map.offset_for_address(kPointerAddressLimit, &offset));
}

GC_TEST(ZForwarding, AttachedArraySitsAfterObject)
{
    constexpr MAddress kStart = 0x50000000;
    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, 0x1000);
    GC_EXPECT_TRUE(fwd != nullptr);
    const size_t objectSize = ZForwarding::AttachedArray::object_size();
    GC_EXPECT_TRUE(objectSize >= sizeof(ZForwarding));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(fwd->entries()),
                 reinterpret_cast<uintptr_t>(fwd) + objectSize);
    GC_EXPECT_EQ(objectSize % sizeof(std::atomic<uint64_t>), static_cast<size_t>(0));
    GC_EXPECT_TRUE((fwd->length() & (fwd->length() - 1)) == 0);

    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), 1);
    GC_EXPECT_FALSE(fwd->claimed().load(std::memory_order_acquire));
    GC_EXPECT_FALSE(fwd->is_done());

    const MAddress from = kStart + 16;
    const MAddress to = kStart + 0x2000;
    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(fwd->find(from), to);
    GC_EXPECT_EQ(fwd->find(kStart + 24), static_cast<MAddress>(0));
    fwd->Destroy();
}

// The provisional table is not a different lifetime object: it enters the same
// three-state ref-count protocol before it is published in the granule map.
// This couples the two pieces changed together by the provisional-table port.
GC_TEST(ZForwarding, PageUsesRefCountProtocol)
{
    constexpr MAddress kStart = 0x58000000;
    ZForwarding* fwd = ZForwarding::alloc(1, kStart, kStart, 0x1000, nullptr, 7);
    GC_EXPECT_TRUE(fwd != nullptr);
    GC_EXPECT_EQ(fwd->page_life_id(), static_cast<RegionLifeId>(7));
    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), 1);

    GC_EXPECT_TRUE(fwd->retain_page());
    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), 2);
    fwd->release_page();
    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), 1);

    GC_EXPECT_TRUE(fwd->claim());
    fwd->in_place_relocation_claim_page();
    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), -1);
    GC_EXPECT_FALSE(fwd->retain_page());
    fwd->mark_done();
    GC_EXPECT_TRUE(fwd->is_done());
    fwd->release_page();
    GC_EXPECT_EQ(fwd->ref_count().load(std::memory_order_acquire), 0);
    GC_EXPECT_FALSE(fwd->retain_page());
    fwd->Destroy();
}

GC_TEST(ZForwardingTable, kZfwdTableConsumeOn)
{
    static_assert(ForwardingTable::kZfwdTableConsume,
                  "step ② IsFromObject consumes ZForwardingTable::get (PORT_ZFORWARDING.md §六)");
    GC_EXPECT_TRUE(ForwardingTable::kZfwdTableConsume);
}

GC_TEST(ZForwardingTable, PageReleaseKeepsEntriesUntilMapRemoval)
{
    constexpr MAddress kStart = 0x60000000;
    constexpr size_t kSize = 0x1000;
    ZGranuleMap<ZForwarding*> entries;
    GC_EXPECT_TRUE(entries.Initialize(kStart, 4 * kSize, kSize));

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    const MAddress from = kStart + 16;
    const MAddress to = 0x70000000;
    zoffset start;
    zoffset fromOffset;
    GC_EXPECT_TRUE(entries.offset_for_address(kStart, &start));
    GC_EXPECT_TRUE(entries.offset_for_address(from, &fromOffset));
    entries.put(start, kSize, fwd);

    fwd->release_page();
    fwd->detach_page();
    GC_EXPECT_TRUE(entries.get(start) == fwd);

    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(entries.get(fromOffset)->find(from), to);

    entries.put(start, kSize, nullptr);
    fwd->Destroy();
}

// ZGeneration::reset_relocation_set / ZRelocationSet::reset. Source-page
// release leaves forwarding available; the generation reset removes it.
GC_TEST(ZForwardingTable, GenerationResetOwnsForwardingLifetime)
{
    GcHeapFixture fixture;
    fixture.InstallPageOwner(fixture.region0);
    ZForwarding* forwarding = ForwardingTable::GetEntries(fixture.heapStart);
    GC_EXPECT_TRUE(forwarding != nullptr);
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress to = reinterpret_cast<MAddress>(fixture.obj1);
    GC_EXPECT_EQ(forwarding->insert(from, to), to);
    forwarding->release_page();
    forwarding->detach_page();
    forwarding->mark_done();
    ForwardingTable::ClearPageOwner(fixture.region0);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), to);
    const Generation owner = fixture.region0->GetOwnerGeneration();
    const Generation other = owner == Generation::Young ? Generation::Old : Generation::Young;
    ForwardingTable::ResetRelocationSet(other);
    GC_EXPECT_EQ(ForwardingTable::FindTo(from), to);
    ForwardingTable::ResetRelocationSet(owner);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from) == nullptr);
}
