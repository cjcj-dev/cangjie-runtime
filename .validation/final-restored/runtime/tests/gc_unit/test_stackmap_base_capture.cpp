// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>
#include "StackMap/StackMap.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;

#if defined(__x86_64__)
namespace {
// Encoded metadata exercises the shipped header implementation. Managed
// coverage separately verifies the compiler-produced map and product SO.
struct MapBits {
    U8 data[128] {};
    U32 bit = 0;
    void Put(U32 value, U32 width)
    {
        for (U32 i = 0; i < width; ++i, ++bit) {
            data[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
        }
    }
    void Var(U32 value)
    {
        if (value <= 11) { Put(value, 4); }
        else { Put(12, 4); Put(value, 8); }
    }
};

void RunCapture(bool registerBase, bool oop, bool derivedRegister, U32 derivedCount, bool missing = false)
{
    MapBits regs;
    regs.Var(2); regs.Var(16);
    regs.Put(1u << Register::RBX, 16);
    regs.Put(1u << Register::R12, 16);
    RegTable regTable(regs.data, 0);
    MapBits slots;
    slots.Var(2); slots.Var(8); slots.Var(2);
    slots.Put(0, 8); slots.Put(1, 2); // base at fp
    slots.Put(248, 8); slots.Put(derivedCount == 2 ? 3 : 1, 2); // fp-8, fp-16
    SlotTable slotTable(slots.data, 0, 0);
    MapBits pairs;
    pairs.Var(1); pairs.Put(derivedRegister ? 2 : 0, 2); pairs.Put(derivedRegister ? 0 : 2, 2);
    DerivedPtrTable derivedTable(BitsManager(pairs.data, 0), 2, 2);
    IdxSet idx;
    if (registerBase) {
        if (oop) { idx.oopRegIdx = 1; } else { idx.regIdx = 1; }
    } else {
        if (oop) { idx.oopSlotIdx = 1; } else { idx.slotIdx = 1; }
    }
    idx.derivedPtrIdx = derivedCount ? 1 : 0;
    StackMapEntry entry(idx, regTable, slotTable, LineNumTable(), derivedTable, derivedCount ? 1 : 0, true);
    uintptr_t frame[4] = {0x10018, 0x10008, 0x10000, 0};
    uintptr_t baseReg = 0x10000;
    uintptr_t derivedReg = 0x10008;
    RegSlotsMap locations;
    if (!missing) { locations.Insert(Register::RBX, &RootSlotAt(reinterpret_cast<Uptr>(&baseReg))); }
    locations.Insert(Register::R12, &RootSlotAt(reinterpret_cast<Uptr>(&derivedReg)));
    HeapReferenceMap map(true, reinterpret_cast<Uptr>(&frame[2]), entry, PrologueRegisterClosure());
    U32 derivedVisits = 0;
    bool originalBase = true;
    DerivedPtrVisitor derived = [&](BasePtrType base, DerivedSlot& slot) {
        ++derivedVisits;
        originalBase = originalBase && raw(base) == 0x10000;
        const auto offset = raw(slot.LoadDerived()) - raw(base);
        RootSlot moved;
        StorePlain(moved, to_zaddress(0x20000));
        RebaseDerived(slot, moved, offset);
    };
    map.VisitDerivedPtr(derived, nullptr, locations);
    const bool baseAvailable = locations.HasReg(Register::RBX);
    const bool derivedConsumed = !locations.HasReg(Register::R12);
    U32 baseVisits = 0;
    RootVisitor ordinary = [&](RootSlot& slot) {
        ++baseVisits;
        StorePlain(slot, to_zaddress(0x20000));
    };
    const bool accepted = map.VisitRegRoots(ordinary, nullptr, locations);
    map.VisitSlotRoots(ordinary, nullptr);
    std::fprintf(stderr, "BASE_CAPTURE_TARGET reg=%d oop=%d derived_reg=%d count=%u missing=%d available=%d accepted=%d base_visits=%u derived_visits=%u\n",
                 registerBase, oop, derivedRegister, derivedCount, missing, baseAvailable, accepted, baseVisits, derivedVisits);
    // Assert the ordinary product result first, so the old destructive capture
    // fails at the target rather than at a preliminary map-existence check.
    GC_EXPECT_EQ(baseVisits, missing ? 0u : 1u);
    GC_EXPECT_EQ(accepted, !missing);
    if (registerBase) { GC_EXPECT_EQ(baseAvailable, !missing); }
    GC_EXPECT_TRUE(originalBase);
    GC_EXPECT_EQ(derivedVisits, derivedCount);
    if (derivedRegister && derivedCount) {
        GC_EXPECT_TRUE(derivedConsumed);
        GC_EXPECT_EQ(derivedReg, uintptr_t(0x20008));
    } else if (derivedCount) {
        GC_EXPECT_EQ(frame[1], uintptr_t(0x20008));
        if (derivedCount == 2) { GC_EXPECT_EQ(frame[0], uintptr_t(0x20018)); }
    }
    if (registerBase && !missing) {
        GC_EXPECT_FALSE(locations.HasReg(Register::RBX));
        GC_EXPECT_EQ(baseReg, uintptr_t(0x20000));
        // The next frame supplies a new saved location after real consumption.
        PrologueRegisterClosure next;
        next.calleeSaved.push_back(0); next.offset.push_back(1);
        next.RecordCalleeSaved(locations, reinterpret_cast<Uptr>(&frame[2]));
        GC_EXPECT_TRUE(locations.HasReg(Register::RBX));
        GC_EXPECT_TRUE(locations.addrMap[Register::RBX] == &RootSlotAt(reinterpret_cast<Uptr>(&frame[1])));
    }
}
}

GC_TEST(StackMapBaseCapture, RegisterWithoutDerived) { RunCapture(true, false, false, 0); }
GC_TEST(StackMapBaseCapture, RegisterWithDerivedSlot) { RunCapture(true, false, false, 1); }
GC_TEST(StackMapBaseCapture, OopRegisterWithDerivedSlot) { RunCapture(true, true, false, 1); }
GC_TEST(StackMapBaseCapture, SharedRegisterBase) { RunCapture(true, false, false, 2); }
GC_TEST(StackMapBaseCapture, SlotWithDerivedRegister) { RunCapture(false, false, true, 1); }
GC_TEST(StackMapBaseCapture, OopSlotWithDerivedRegister) { RunCapture(false, true, true, 1); }
GC_TEST(StackMapBaseCapture, MissingRegisterStillRejected) { RunCapture(true, false, false, 0, true); }
#endif
