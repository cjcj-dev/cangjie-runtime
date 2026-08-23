// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/ForwardingTable.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ClearEntries has two independent obligations: exchange the owner slot so the
// table enters the retired generation, and clear every slot in a multi-granule
// span.  This target links those public entry points from the product runtime.
GC_TEST(ForwardingTableProduct, ClearEntriesRetiresAndClearsWholeSpan)
{
    constexpr MAddress kStart = 0x40000000;
    constexpr size_t kGranule = 0x1000;
    constexpr size_t kSpan = 2 * kGranule;
    ForwardingTable::Initialize(kStart, 4 * kGranule, kGranule);

    ZForwarding* forwarding = ZForwarding::Create(4, kStart, kStart, kSpan);
    GC_EXPECT_TRUE(forwarding != nullptr);
    ForwardingTable::insert(forwarding);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(kStart) == forwarding);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(kStart + kGranule) == forwarding);

    ForwardingTable::ClearEntries(kStart, kSpan);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(kStart) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(kStart + kGranule) == nullptr);
    GC_EXPECT_TRUE(ForwardingTable::RetiredCovers(kStart, kSpan));

    ForwardingTable::DropRetiredCovering(kStart, kSpan);
    GC_EXPECT_FALSE(ForwardingTable::RetiredCovers(kStart, kSpan));
}
