// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U1 / U2 — colour / address invariants (HotSpot test_zAddress.cpp shape).
// Defect anchors:
//   U1 STACK_ROOTS_STAY_PLAIN (layers 2/3/4) — root slots must hold plain
//   U2 g_cjLoadBadMask bit layout (48,49,51-53 + young/old mark)

#include "Common/ColourMask.h"
#include "Common/ColourTypes.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

// Address occupies bits 0..47 (ColourTypes.h uncolor_bits / RefField.h).
constexpr Uptr kAddrMask = (Uptr(1) << 48) - 1u;
constexpr Uptr kSampleAddr = Uptr(0x00007f12'34567000ULL); // 8-byte aligned, in 48-bit space

// Initial load-bad mask (BaseObject.cpp / WCollector::set_good_masks with Remapped00 current).
constexpr unsigned long kInitialLoadBadMask =
    static_cast<unsigned long>(TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ ZPointerRemapped00));

// Paint address with one-hot remap colour + optional mark bits (production colour write shape).
constexpr Uptr Color(Uptr addr, Uptr colourBits)
{
    return (addr & kAddrMask) | colourBits;
}

// Model of "root slot may only store plain" — production write path peels colour for non-heap.
constexpr bool IsPlainRootValue(Uptr v)
{
    return (v & ~kAddrMask) == 0;
}

// Model of load-good: no bit set in g_cjLoadBadMask.
constexpr bool IsLoadGood(Uptr v, unsigned long badMask)
{
    return (v & static_cast<Uptr>(badMask)) == 0;
}

} // namespace

// U2: uncolor(color(p)) == p for every remap one-hot and mark-bit combo we publish.
GC_TEST(ColourAddress, UncolorRoundTripAllRemapOneHot)
{
    const Uptr remapBits[] = { ZPointerRemapped00, ZPointerRemapped01, ZPointerRemapped10, ZPointerRemapped11 };
    for (Uptr r : remapBits) {
        zpointer coloured = to_zpointer(Color(kSampleAddr, r));
        zaddress_unsafe stripped = uncolor_bits(coloured);
        GC_EXPECT_EQ(raw(stripped), kSampleAddr);
    }
}

GC_TEST(ColourAddress, UncolorRoundTripWithMarkBits)
{
    const Uptr markCombos[] = {
        0u,
        MARKED_YOUNG_0,
        MARKED_YOUNG_1,
        MARKED_OLD_0,
        MARKED_OLD_1,
        MARKED_YOUNG_0 | MARKED_OLD_0,
        MARKED_YOUNG_1 | MARKED_OLD_1,
    };
    for (Uptr m : markCombos) {
        Uptr coloured = Color(kSampleAddr, ZPointerRemapped00 | m);
        GC_EXPECT_EQ(raw(uncolor_bits(to_zpointer(coloured))), kSampleAddr);
    }
}

// U2: load-bad mask rejects every non-current remap one-hot (initial current = Remapped00).
GC_TEST(ColourAddress, LoadBadMaskRejectsStaleRemap)
{
    // Current good: Remapped00 only (no tagged bits).
    Uptr good = Color(kSampleAddr, ZPointerRemapped00);
    GC_EXPECT_TRUE(IsLoadGood(good, kInitialLoadBadMask));

    const Uptr stale[] = { ZPointerRemapped01, ZPointerRemapped10, ZPointerRemapped11 };
    for (Uptr s : stale) {
        Uptr bad = Color(kSampleAddr, s);
        GC_EXPECT_FALSE(IsLoadGood(bad, kInitialLoadBadMask));
    }
}

// U2: a stale remap colour is always load-bad (no isTagged bit).
GC_TEST(ColourAddress, StaleRemapAlwaysLoadBad)
{
    Uptr v = Color(kSampleAddr, ZPointerRemapped01);
    GC_EXPECT_FALSE(IsLoadGood(v, kInitialLoadBadMask));
    GC_EXPECT_EQ(raw(uncolor_bits(to_zpointer(v))), kSampleAddr);
}

// U1: root slot discipline — coloured value is not a legal plain root payload.
GC_TEST(ColourAddress, RootSlotRejectsColouredValue)
{
    Uptr coloured = Color(kSampleAddr, ZPointerRemapped00);
    GC_EXPECT_FALSE(IsPlainRootValue(coloured));

    Uptr plain = raw(uncolor_bits(to_zpointer(coloured)));
    GC_EXPECT_TRUE(IsPlainRootValue(plain));
    GC_EXPECT_EQ(plain, kSampleAddr);
}

// U1: after peel, high colour bits must be zero (STACK_ROOTS_STAY_PLAIN write-back shape).
GC_TEST(ColourAddress, PeelForRootWriteBackClearsHighBits)
{
    Uptr coloured = Color(kSampleAddr, REMAP_COLOUR_MASK | MARKED_YOUNG_1 | MARKED_OLD_1);
    Uptr peeled = raw(uncolor_bits(to_zpointer(coloured)));
    GC_EXPECT_TRUE(IsPlainRootValue(peeled));
    GC_EXPECT_EQ(peeled, kSampleAddr);
}

// Layout sanity: address mask and colour masks are disjoint (defect if they overlap).
GC_TEST(ColourAddress, AddressAndColourMasksDisjoint)
{
    GC_EXPECT_EQ(kAddrMask & REMAP_COLOUR_MASK, 0u);
    GC_EXPECT_EQ(kAddrMask & MARKED_YOUNG_MASK, 0u);
    GC_EXPECT_EQ(kAddrMask & MARKED_OLD_MASK, 0u);
    GC_EXPECT_EQ(kAddrMask & TAGGED_BITS_MASK, 0u);
}

// Eth: colour bit-field encode/decode matrix (JDK test_zBitField spirit).
// Product colour one-hots must round-trip through paint/uncolor without cross-talk.
GC_TEST(ColourAddress, BitFieldRemapOneHotMatrix)
{
    const Uptr remapBits[] = { ZPointerRemapped00, ZPointerRemapped01, ZPointerRemapped10, ZPointerRemapped11 };
    for (size_t i = 0; i < 4; ++i) {
        Uptr coloured = Color(kSampleAddr, remapBits[i]);
        GC_EXPECT_EQ(raw(uncolor_bits(to_zpointer(coloured))), kSampleAddr);
        // Exactly one remap one-hot set in the colour field.
        Uptr colourOnly = coloured & REMAP_COLOUR_MASK;
        GC_EXPECT_EQ(colourOnly, remapBits[i]);
        for (size_t j = 0; j < 4; ++j) {
            if (i == j) {
                GC_EXPECT_EQ(colourOnly & remapBits[j], remapBits[j]);
            } else {
                GC_EXPECT_EQ(colourOnly & remapBits[j], 0u);
            }
        }
    }
}

// Eth: mark young/old bits encode independently of remap one-hot.
GC_TEST(ColourAddress, BitFieldMarkBitsIndependentOfRemap)
{
    Uptr base = Color(kSampleAddr, ZPointerRemapped10 | MARKED_YOUNG_1 | MARKED_OLD_0);
    GC_EXPECT_EQ(raw(uncolor_bits(to_zpointer(base))), kSampleAddr);
    GC_EXPECT_EQ(base & REMAP_COLOUR_MASK, ZPointerRemapped10);
    GC_EXPECT_EQ(base & MARKED_YOUNG_MASK, MARKED_YOUNG_1);
    GC_EXPECT_EQ(base & MARKED_OLD_MASK, MARKED_OLD_0);
}
