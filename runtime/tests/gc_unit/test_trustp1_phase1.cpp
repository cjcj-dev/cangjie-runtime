// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// TRUST_STATE_KILL_PLAN Phase 1 contracts (header-level positive/negative).
// Runtime inject positive control lives in PlainCensus (MRT_GCV2_PLAIN_WRITE_INJECT=1).

#include "Common/ColourMask.h"
#include "Common/ColourTypes.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr Uptr kAddrMask = (Uptr(1) << 48) - 1u;
constexpr Uptr kSampleAddr = Uptr(0x00007f12'34567000ULL);
constexpr Uptr kColourMetaMask = REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK;

// Model of Phase-1 TryUntag / HeapSlot write-back: install current colour, not plain.
constexpr Uptr ModelHeapSlotWriteback(Uptr addr, Uptr currentColour)
{
    return (addr & kAddrMask) | (currentColour & kColourMetaMask);
}

// Model of RootSlot write-back under PLAIN_ROOTS=1: address bits only.
constexpr Uptr ModelRootSlotWritebackPlain(Uptr addr)
{
    return addr & kAddrMask;
}

// Census plain definition: non-null address bits, zero colour metadata.
constexpr bool IsPlainHeapRef(Uptr v)
{
    if ((v & kAddrMask) == 0) {
        return false;
    }
    return (v & kColourMetaMask) == 0;
}

} // namespace

// ① positive: HeapSlot write-back after untag must be coloured (not plain).
GC_TEST(TrustP1, TryUntagHeapSlotWritebackIsColoured)
{
    Uptr current = ZPointerRemapped00 | MARKED_YOUNG_0 | MARKED_OLD_0;
    Uptr written = ModelHeapSlotWriteback(kSampleAddr, current);
    GC_EXPECT_FALSE(IsPlainHeapRef(written));
    GC_EXPECT_EQ(written & kAddrMask, kSampleAddr);
    GC_EXPECT_NE(written & kColourMetaMask, 0u);
}

// ① negative: plain untag write-back would be counted as plainHeapRefSlots residual.
GC_TEST(TrustP1, PlainUntagWritebackWouldBeCensusHit)
{
    Uptr plain = kSampleAddr; // old TryUntag shape RefField<>(target)
    GC_EXPECT_TRUE(IsPlainHeapRef(plain));
}

// ② positive: RootSlot plain write-back stays plain (legal; not census-counted).
GC_TEST(TrustP1, RootSlotWritebackPlainIsPlain)
{
    Uptr plain = ModelRootSlotWritebackPlain(kSampleAddr | ZPointerRemapped01);
    GC_EXPECT_TRUE(IsPlainHeapRef(plain) || (plain & kAddrMask) == kSampleAddr);
    GC_EXPECT_EQ(plain & kColourMetaMask, 0u);
    GC_EXPECT_EQ(plain, kSampleAddr);
}

// ② negative: HeapSlot must not use the plain root write-back shape.
GC_TEST(TrustP1, HeapSlotMustNotUseRootPlainShape)
{
    Uptr rootShape = ModelRootSlotWritebackPlain(kSampleAddr);
    Uptr heapShape = ModelHeapSlotWriteback(kSampleAddr, ZPointerRemapped00);
    GC_EXPECT_TRUE(IsPlainHeapRef(rootShape));
    GC_EXPECT_FALSE(IsPlainHeapRef(heapShape));
}

// ③ positive: derived interior plain is legal (derived_legal column), distinct from K1.
GC_TEST(TrustP1, DerivedInteriorPlainIsDistinctFromK1ObjectRoot)
{
    // Interior tip = base+8, still plain bits — product tags FixMinorInterior.
    Uptr interior = kSampleAddr + 8;
    GC_EXPECT_TRUE(IsPlainHeapRef(interior));
    // Object root with colour is not plain.
    Uptr colouredRoot = ModelHeapSlotWriteback(kSampleAddr, ZPointerRemapped00);
    GC_EXPECT_FALSE(IsPlainHeapRef(colouredRoot));
}

// ④ positive control model: inject plain then census counts 1.
GC_TEST(TrustP1, CensusPlainDefinitionMatchesInjectShape)
{
    Uptr coloured = ModelHeapSlotWriteback(kSampleAddr, ZPointerRemapped00 | MARKED_YOUNG_0);
    Uptr injected = coloured & kAddrMask; // inject peels colour meta
    GC_EXPECT_FALSE(IsPlainHeapRef(coloured));
    GC_EXPECT_TRUE(IsPlainHeapRef(injected));
}

// ④ null is never a plain residual.
GC_TEST(TrustP1, NullIsNotPlainResidual)
{
    GC_EXPECT_FALSE(IsPlainHeapRef(0));
    GC_EXPECT_FALSE(IsPlainHeapRef(ZPointerRemapped00)); // colour-only null-ish
}
