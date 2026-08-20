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

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZGranuleMap, GetPutRemove)
{
    constexpr MAddress kStart = 0x40000000;
    constexpr size_t kSize = 0x1000;
    ZGranuleMap<ZForwarding*> map;
    GC_EXPECT_TRUE(map.Initialize(kStart, 4 * kSize, kSize));

    ZForwarding* fwd = ZForwarding::Create(4, kStart, kStart, kSize);
    GC_EXPECT_TRUE(fwd != nullptr);
    map.put(kStart, kSize, fwd);
    GC_EXPECT_TRUE(map.get(kStart) == fwd);
    GC_EXPECT_TRUE(map.get(kStart + 8) == fwd);
    GC_EXPECT_TRUE(map.get(kStart + kSize) == nullptr);

    map.put(kStart, kSize, nullptr);
    GC_EXPECT_TRUE(map.get(kStart) == nullptr);
    fwd->Destroy();
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
    membership.put(kStart, kSize, fwd);
    entries.put(kStart, kSize, fwd);

    membership.put(kStart, kSize, nullptr);
    GC_EXPECT_TRUE(membership.get(kStart) == nullptr);
    GC_EXPECT_TRUE(entries.get(kStart) == fwd);

    const MAddress from = kStart + 16;
    const MAddress to = 0x70000000;
    GC_EXPECT_EQ(fwd->insert(from, to), to);
    GC_EXPECT_EQ(entries.get(from)->find(from), to);

    entries.put(kStart, kSize, nullptr);
    fwd->Destroy();
}
