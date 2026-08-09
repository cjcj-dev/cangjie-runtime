// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U5 / U6 — interior base recovery + PlausibleManagedObjectGate tip rules (models).
// Defect anchors: floor knife ① si_addr=0x208 / RawArray+8 (fixinput);
// nilclass tip-small-int / tip-in-heap.

#include <cstdint>
#include <cstring>

#include "gc_unittest.hpp"

using namespace MapleRuntime::GcUnit;

namespace {

// StateWord tip lives at object base; interior RawArray+8 is not a valid object tip site.
// ClassifyInteriorOffset production uses tip patterns; here we model the gate tip checks only.
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x10000u; // matches Collector.cpp gate floor
constexpr uintptr_t kHeapStart = 0x40000000u;
constexpr uintptr_t kHeapEnd = 0x50000000u;
constexpr uintptr_t kAddrAlignMask = 0x7u; // StateWord::ADDRESS_ALIGN_MASK typical 8-byte

constexpr bool IsHeapAddress(uintptr_t addr)
{
    return addr >= kHeapStart && addr < kHeapEnd;
}

// Model of Collector::PlausibleManagedObjectGate tip branch (Collector.cpp:290-304).
// Returns reject reason or nullptr if tip looks plausible (region checks omitted here).
const char* TipGateReason(uintptr_t tipAddr)
{
    if (tipAddr == 0) {
        return "null-tip";
    }
    if (tipAddr < kMinPlausibleTypeInfoAddr) {
        return "tip-small-int";
    }
    if ((tipAddr & kAddrAlignMask) != 0) {
        return "tip-misaligned";
    }
    if (IsHeapAddress(tipAddr)) {
        return "tip-in-heap";
    }
    return nullptr;
}

// U5 model: RawArray+8 interior — tip word at interior is often a small int (length) or heap ptr.
// Recover base by subtracting known interior offset 8 when tip-small-int.
constexpr uintptr_t TryRecoverInteriorBaseOff8(uintptr_t interior, uintptr_t tipAtInterior)
{
    if (tipAtInterior != 0 && tipAtInterior < kMinPlausibleTypeInfoAddr) {
        return interior - 8;
    }
    return 0;
}

} // namespace

// U6: tip-small-int rejected (classic RawArray length word misread as tip).
GC_TEST(ObjectGate, TipSmallIntRejected)
{
    GC_EXPECT_EQ(std::strcmp(TipGateReason(0), "null-tip"), 0);
    GC_EXPECT_EQ(std::strcmp(TipGateReason(42), "tip-small-int"), 0);
    GC_EXPECT_EQ(std::strcmp(TipGateReason(8), "tip-small-int"), 0);
}

// U6: tip-in-heap rejected (interior into another managed object).
GC_TEST(ObjectGate, TipInHeapRejected)
{
    uintptr_t heapTip = kHeapStart + 0x1000;
    GC_EXPECT_EQ(std::strcmp(TipGateReason(heapTip), "tip-in-heap"), 0);
}

// U6: aligned non-heap tip accepted by tip branch.
GC_TEST(ObjectGate, PlausibleTipAccepted)
{
    uintptr_t metaTip = 0x7f0000001000ull; // outside mock heap, aligned
    GC_EXPECT_TRUE(TipGateReason(metaTip) == nullptr);
}

// U5: RawArray+8 interior recovers base when tip word is small int (length).
GC_TEST(ObjectGate, RawArrayPlus8RecoversBase)
{
    uintptr_t base = kHeapStart + 0x200;
    uintptr_t interior = base + 8; // &MArray::length shape
    uintptr_t lengthWord = 16;     // tip-small-int at interior
    uintptr_t recovered = TryRecoverInteriorBaseOff8(interior, lengthWord);
    GC_EXPECT_EQ(recovered, base);
    // Gate on interior tip rejects.
    GC_EXPECT_EQ(std::strcmp(TipGateReason(lengthWord), "tip-small-int"), 0);
}

// U5: non-interior (plausible tip) does not force off-8 recovery.
GC_TEST(ObjectGate, NonInteriorNoFalseRecover)
{
    uintptr_t obj = kHeapStart + 0x300;
    uintptr_t metaTip = 0x7f0000002000ull;
    GC_EXPECT_EQ(TryRecoverInteriorBaseOff8(obj, metaTip), 0u);
}
