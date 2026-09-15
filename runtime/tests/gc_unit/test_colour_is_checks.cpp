// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zAddress.hpp"
#include "Common/ColourEncoding.h"
#include "ObjectModel/RefField.h"
#include "gc_unittest.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// The exhaustive predicate/phase matrix lives in test_colour_address.cpp,
// ported from ZGC test_zAddress.cpp:177-435. These exercise product encoders
// and their results, not a second copy of the encoding formula.
GC_TEST(ZAddress, UncolorRoundTrip)
{
    ZGlobalsPointers::initialize();
    const auto address = static_cast<zaddress>(ZAddressHeapBase | 0x1000);
    for (int old = 0; old < 2; ++old) {
        for (int young = 0; young < 2; ++young) {
            const zpointer pointer = ZAddress::store_good(address);
            GC_EXPECT_TRUE(is_valid(pointer));
            GC_EXPECT_EQ(raw(ZPointer::uncolor(pointer)), raw(address));
            GC_EXPECT_EQ(raw(ZPointer::uncolor_store_good(pointer)), raw(address));
            GC_EXPECT_EQ(raw(ZOffset::address(ZAddress::offset(address))), raw(address));
            ZGlobalsPointers::flip_young_relocate_start();
            GC_EXPECT_TRUE(ZPointer::is_load_bad(pointer));
            GC_EXPECT_EQ(raw(ZPointer::uncolor_unsafe(pointer)), raw(address));
        }
        ZGlobalsPointers::flip_old_relocate_start();
    }
}

GC_TEST(ZAddress, FinalizableFlip)
{
    ZGlobalsPointers::initialize();
    const auto address = static_cast<zaddress>(ZAddressHeapBase | 0x1000);
    const auto previous = ZAddress::store_good(address);
    const auto first = ZAddress::finalizable_good(address, previous);
    GC_EXPECT_TRUE(is_valid(first));
    GC_EXPECT_TRUE(ZPointer::is_marked_finalizable(first));
    GC_EXPECT_FALSE(ZPointer::is_marked_old(first));
    ZGlobalsPointers::flip_old_mark_start();
    GC_EXPECT_FALSE(ZPointer::is_marked_finalizable(first));
    const auto next = ZAddress::finalizable_good(address, previous);
    GC_EXPECT_EQ(raw(next) & ZPointerFinalizableMask, ZPointerFinalizable1);
    GC_EXPECT_TRUE(ZPointer::is_marked_finalizable(next));
}

GC_TEST(ZAddress, AddressValidity)
{
    ZGlobalsPointers::initialize();
    GC_EXPECT_TRUE(is_valid(static_cast<zaddress>(ZAddressHeapBase | 8)));
    GC_EXPECT_FALSE(is_valid(static_cast<zaddress>(8)));
    GC_EXPECT_FALSE(is_valid(static_cast<zaddress>(ZAddressHeapBase | 1)));
    GC_EXPECT_FALSE(is_valid(static_cast<zaddress>(ZAddressHeapBase + ZAddressOffsetMax)));
    GC_EXPECT_TRUE(is_valid(zaddress::null));
    GC_EXPECT_FALSE(is_valid(zpointer::null));
    GC_EXPECT_TRUE(is_valid(color_null()));
}

GC_TEST(ColourIsChecks, BarrierColouredNullAndDoubleRemembered)
{
    ZGlobalsPointers::initialize();
    const auto colouredNull = ZAddress::store_good(zaddress::null);
    GC_EXPECT_EQ(ClassifySlotWord(raw(colouredNull)), SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(is_null_any(colouredNull));
    GC_EXPECT_TRUE(ZPointer::is_store_good(colouredNull));
    GC_EXPECT_FALSE(ZPointer::is_store_good(zpointer::null));
    const auto address = static_cast<zaddress>(ZAddressHeapBase | 0x1000);
    const auto loadHealed = ZAddress::load_good(address, ZAddress::store_good(address));
    GC_EXPECT_EQ(ClassifySlotWord(raw(loadHealed)), SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(ZPointer::is_load_good(loadHealed));
    GC_EXPECT_TRUE(ZPointer::is_mark_good(loadHealed));
    GC_EXPECT_FALSE(ZPointer::is_store_good(loadHealed));
    GC_EXPECT_EQ(ClassifySlotWord(raw(loadHealed) & ~ZPointerRememberedMask), SlotWordVerdict::kIllegal);
}

GC_TEST(ColourIsChecks, BarrierSelfHealUpgradeAndCompetingStore)
{
    ZGlobalsPointers::initialize();
    const auto address = static_cast<zaddress>(ZAddressHeapBase | 0x1000);
    const zpointer old = ZAddress::store_good(address);
    ZGlobalsPointers::flip_young_relocate_start();
    const zpointer healed = ZAddress::load_good(address, old);
    auto fast = [](zpointer word) { return ZPointer::is_load_good_or_null(word); };
    HeapSlot<> slot(old);
    GC_EXPECT_TRUE(ZgcSelfHeal(slot, old, healed, fast, HealSite::BarrierReadReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(healed));
    const auto writer = ZAddress::store_good(static_cast<zaddress>(ZAddressHeapBase | 0x2000));
    slot.StoreColoured(writer);
    GC_EXPECT_FALSE(ZgcSelfHeal(slot, old, healed, fast, HealSite::BarrierReadReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(writer));
    slot.StoreColoured(old);
    GC_EXPECT_FALSE(ZgcSelfHeal(slot, old, color_null(), fast, HealSite::BarrierReadReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(old));
}
