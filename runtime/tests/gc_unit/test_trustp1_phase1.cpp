// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zAddress.hpp"
#include "ObjectModel/RefField.h"
#include "Common/ColourEncoding.h"
#include "gc_unittest.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
zaddress SampleAddress()
{
    ZGlobalsPointers::initialize();
    return static_cast<zaddress>(ZAddressHeapBase | 0x1000);
}
}
GC_TEST(TrustP1, HeapSlotWritebackIsColoured)
{
    const auto address = SampleAddress();
    HeapSlot<> slot(zpointer::null);
    slot.StoreColoured(ZAddress::store_good(address));
    GC_EXPECT_EQ(ClassifySlotWord(raw(slot.GetFieldValue())), SlotWordVerdict::kColoured);
    GC_EXPECT_EQ(raw(slot.GetTargetObject()), raw(address));
}
GC_TEST(TrustP1, PlainWritebackIsEncodingIllegal)
{
    const auto address = SampleAddress();
    GC_EXPECT_EQ(ClassifySlotWord(raw(address)), SlotWordVerdict::kIllegal);
}
GC_TEST(TrustP1, RootSlotWritebackPlainIsPlain)
{
    const auto address = SampleAddress();
    RootSlot slot;
    StorePlain(slot, address);
    GC_EXPECT_EQ(raw(slot.LoadPlain()), raw(address));
    GC_EXPECT_TRUE(is_valid(static_cast<zaddress>(raw(slot.LoadPlain()))));
}
GC_TEST(TrustP1, HeapSlotMustNotUseRootPlainShape)
{
    const auto address = SampleAddress();
    RootSlot root;
    StorePlain(root, address);
    HeapSlot<> heap(ZAddress::store_good(address));
    GC_EXPECT_NE(raw(root.LoadPlain()), raw(heap.GetFieldValue()));
    GC_EXPECT_EQ(ClassifySlotWord(raw(root.LoadPlain())), SlotWordVerdict::kIllegal);
    GC_EXPECT_EQ(ClassifySlotWord(raw(heap.GetFieldValue())), SlotWordVerdict::kColoured);
}
GC_TEST(TrustP1, DerivedInteriorPlainIsDistinctFromObjectRoot)
{
    const auto address = SampleAddress();
    RootSlot base;
    StorePlain(base, address);
    DerivedSlot derived;
    RebaseDerived(derived, base, 3);
    GC_EXPECT_EQ(raw(derived.LoadDerived()), raw(address) + 3);
    GC_EXPECT_FALSE(is_valid(static_cast<zpointer>(raw(derived.LoadDerived()))));
}
GC_TEST(TrustP1, SlotClassifierRejectsPlainShape)
{
    const auto address = SampleAddress();
    const auto colored = ZAddress::store_good(address);
    GC_EXPECT_EQ(ClassifySlotWord(raw(colored)), SlotWordVerdict::kColoured);
    const auto uncolored = ZPointer::uncolor_store_good(colored);
    GC_EXPECT_EQ(ClassifySlotWord(raw(uncolored)), SlotWordVerdict::kIllegal);
}
GC_TEST(TrustP1, NullIsNotPlainResidual)
{
    SampleAddress();
    GC_EXPECT_FALSE(IsPlainNonNullSlotWord(0));
    GC_EXPECT_TRUE(is_null_any(color_null()));
    GC_EXPECT_EQ(ClassifySlotWord(raw(color_null())), SlotWordVerdict::kColoured);
}
