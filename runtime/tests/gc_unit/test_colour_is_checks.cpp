// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zAddress.hpp"
#include "Common/ColourEncoding.h"
#include "ObjectModel/RefField.h"
#include "gc_unittest.hpp"
#include "Heap/z/zGeneration.hpp"
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
    GC_EXPECT_TRUE(ZgcSelfHeal(slot, old, healed, fast, HealSite::BarrierCompareAndSwapReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(healed));
    const auto writer = ZAddress::store_good(static_cast<zaddress>(ZAddressHeapBase | 0x2000));
    slot.StoreColoured(writer);
    GC_EXPECT_FALSE(ZgcSelfHeal(slot, old, healed, fast, HealSite::BarrierCompareAndSwapReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(writer));
    slot.StoreColoured(old);
    GC_EXPECT_FALSE(ZgcSelfHeal(slot, old, color_null(), fast, HealSite::BarrierCompareAndSwapReference));
    GC_EXPECT_EQ(raw(slot.GetFieldValue()), raw(old));
}

GC_TEST(ZAddress, GenerationFragmentationPolicy)
{
    GenerationCycle young(GCCycleGeneration::YOUNG);
    GenerationCycle old(GCCycleGeneration::OLD);
    GC_EXPECT_EQ(young.FragmentationLimit(), 25.0);
    GC_EXPECT_EQ(old.FragmentationLimit(), 5.0);
}

#include "gc_heap_fixture.hpp"
extern "C" void MCC_WriteRefField(MapleRuntime::ObjectPtr, MapleRuntime::ObjectPtr, MapleRuntime::RefField<false>*);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadRefField(MapleRuntime::ObjectPtr, MapleRuntime::RefField<false>*);
extern "C" void MCC_WriteStructField(MapleRuntime::ObjectPtr, MapleRuntime::MAddress, size_t,
                                      MapleRuntime::MAddress, size_t, MapleRuntime::GCTib);
extern "C" void CJ_MCC_ReadStructField(MapleRuntime::MAddress, MapleRuntime::ObjectPtr,
                                       MapleRuntime::MAddress, size_t, MapleRuntime::GCTib);

// P01 value-type ABI: stack roots remain plain; mutable global/heap fields
// are colored. Calls enter the runtime ABI exports, not a test encoding model.
GC_TEST(ValueSlotABI, StackScalarWriteIsPlain)
{
    GcHeapFixture fixture;
    RootSlot slot;
    MCC_WriteRefField(fixture.obj0, nullptr, reinterpret_cast<RefField<false>*>(&slot));
    GC_EXPECT_EQ(raw(slot.LoadPlain()), reinterpret_cast<uintptr_t>(fixture.obj0));
}
GC_TEST(ValueSlotABI, StackScalarReadIsPlain)
{
    GcHeapFixture fixture;
    RootSlot slot;
    StorePlain(slot, from_object(fixture.obj0));
    GC_EXPECT_TRUE(CJ_MCC_ReadRefField(nullptr, reinterpret_cast<RefField<false>*>(&slot)) == fixture.obj0);
}
GC_TEST(ValueSlotABI, NullHolderHeapScalarStaysColored)
{
    GcHeapFixture fixture;
    Heap::GetHeap().GetRememberedSet().Initialize(fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    auto& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    slot.StoreColoured(color_null());
    MCC_WriteRefField(fixture.obj0, nullptr, &slot);
    GC_EXPECT_TRUE(ZPointer::is_store_good(slot.GetFieldValue()));
    GC_EXPECT_TRUE(CJ_MCC_ReadRefField(nullptr, &slot) == fixture.obj0);
}
#if defined(__x86_64__)
GC_TEST(ValueSlotABI, GlobalScalarStaysColored)
{
    GcHeapFixture fixture;
    NativeSlot slot(color_null());
    auto* globalBase = reinterpret_cast<ObjectPtr>(uintptr_t(1));
    MCC_WriteRefField(fixture.obj0, globalBase, &slot);
    GC_EXPECT_TRUE(ZPointer::is_store_good(slot.GetFieldValue()));
    GC_EXPECT_TRUE(CJ_MCC_ReadRefField(globalBase, &slot) == fixture.obj0);
}
#endif

GC_TEST(ValueSlotABI, StackStructWriteAndReadStayPlain)
{
    GcHeapFixture fixture;
    struct Payload { uintptr_t reference; uint64_t primitive; };
    Payload source {reinterpret_cast<uintptr_t>(fixture.obj0), 0x12345678}, target {}, copy {};
    GCTib layout {}; layout.tag = SIGN_BIT | 1;
    MCC_WriteStructField(nullptr, reinterpret_cast<MAddress>(&target), sizeof(target),
                         reinterpret_cast<MAddress>(&source), sizeof(source), layout);
    GC_EXPECT_EQ(target.reference, source.reference);
    GC_EXPECT_EQ(target.primitive, source.primitive);
    CJ_MCC_ReadStructField(reinterpret_cast<MAddress>(&copy), nullptr,
                          reinterpret_cast<MAddress>(&target), sizeof(target), layout);
    GC_EXPECT_EQ(copy.reference, source.reference);
    GC_EXPECT_EQ(copy.primitive, source.primitive);
}
GC_TEST(ValueSlotABI, NullHolderHeapStructStaysColored)
{
    GcHeapFixture fixture;
    struct Payload { uintptr_t reference; uint64_t primitive; };
    fixture.typeInfo->SetInstanceSize(sizeof(Payload));
    fixture.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(fixture.obj1) + fixture.obj1->GetSize());
    Payload source {reinterpret_cast<uintptr_t>(fixture.obj0), 0x12345678}, copy {};
    const MAddress destination = reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE;
    GCTib layout {}; layout.tag = SIGN_BIT | 1;
    MCC_WriteStructField(nullptr, destination, sizeof(Payload), reinterpret_cast<MAddress>(&source), sizeof(source), layout);
    GC_EXPECT_TRUE(ZPointer::is_store_good(HeapSlotAt<>(destination).GetFieldValue()));
    CJ_MCC_ReadStructField(reinterpret_cast<MAddress>(&copy), nullptr, destination, sizeof(Payload), layout);
    GC_EXPECT_EQ(copy.reference, source.reference);
    GC_EXPECT_EQ(copy.primitive, source.primitive);
}
