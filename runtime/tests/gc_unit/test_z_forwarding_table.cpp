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

#include <type_traits>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

static_assert(!std::is_convertible_v<MAddress, zoffset>);
static_assert(!std::is_convertible_v<zoffset, MAddress>);
static_assert(!std::is_convertible_v<zpointer, zoffset>);
static_assert(!std::is_convertible_v<zaddress, zoffset>);
static_assert(!std::is_convertible_v<zaddress_unsafe, zoffset>);

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
GC_TEST(ZForwarding, ProvisionalUsesRefCountProtocol)
{
    constexpr MAddress kStart = 0x58000000;
    ZForwarding* fwd = ZForwarding::alloc(1, kStart, kStart, 0x1000, nullptr, 7, true);
    GC_EXPECT_TRUE(fwd != nullptr);
    GC_EXPECT_TRUE(fwd->is_provisional());
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

GC_TEST(ZForwardingTable, MembershipUnlinkKeepsEntries)
{
    constexpr MAddress kStart = 0x60000000;
    constexpr size_t kSize = 0x1000;
    ZGranuleMap<ZForwarding*> membership;
    ZGranuleMap<ZForwarding*> entries;
    GC_EXPECT_TRUE(membership.Initialize(kStart, 4 * kSize, kSize));
    GC_EXPECT_TRUE(entries.Initialize(kStart, 4 * kSize, kSize));

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    const MAddress from = kStart + 16;
    const MAddress to = 0x70000000;
    zoffset start;
    zoffset fromOffset;
    GC_EXPECT_TRUE(membership.offset_for_address(kStart, &start));
    GC_EXPECT_TRUE(entries.offset_for_address(from, &fromOffset));
    membership.put(start, kSize, fwd);
    entries.put(start, kSize, fwd);

    membership.put(start, kSize, nullptr);
    GC_EXPECT_TRUE(membership.get(start) == nullptr);
    GC_EXPECT_TRUE(entries.get(start) == fwd);

    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(entries.get(fromOffset)->find(from), to);

    entries.put(start, kSize, nullptr);
    fwd->Destroy();
}
