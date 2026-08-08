// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_DERIVED_PTR_ROOT_H
#define MRT_DERIVED_PTR_ROOT_H
#include "StackMap/SlotRoot.h"
#include "StackMap/StackMapTable.h"
#ifdef __aarch64__
#include "StackMap/StackMapAarch64.h"
#elif defined (__arm__)
#include "StackMap/StackMapArm.h"
#else
#include "StackMap/StackMapX86.h"
#endif
namespace MapleRuntime {
class DerivedPtr {
public:
    DerivedPtr() = default;
    DerivedPtr(const DerivedPtrTable& derivePtr, const RegTable& reg, const SlotTable& slot, U32 startIdx)
        : derivePtrTable(derivePtr), regTable(reg), slotTable(slot), derivedPtrIdx(startIdx) {}
    ~DerivedPtr() = default;
    bool VisitDerivedPtr(const DerivedPtrVisitor& visitor, const DerivedPtrDebugVisitor debugVisitor,
                         RegSlotsMap& regSlotsMap, BasePtrType basePtr, Uptr fp)
    {
        if (LIKELY(derivedPtrIdx == 0)) {
            return false;
        }
        DerivedPtrPair idxPair = derivePtrTable.GetDerivePair(derivedPtrIdx - 1);
        U32 regIdx = idxPair.first;
        U32 slotIdx = idxPair.second;
        if (!is_null(basePtr)) {
            if (regIdx != 0) {
                VisitRegDerivedPtr(visitor, debugVisitor, regSlotsMap, basePtr, regIdx - 1);
            }
            if (slotIdx != 0) {
                VisitSlotDerivedPtr(visitor, debugVisitor, basePtr, fp, slotIdx - 1);
            }
        }
        derivedPtrIdx++;
        return true;
    }
    bool VisitDerivedPtrForStackGrow(const DerivedPtrVisitor& visitor, const DerivedPtrDebugVisitor debugVisitor,
                                     RegSlotsMap& regSlotsMap, BasePtrType basePtr, Uptr fp)
    {
        if (LIKELY(derivedPtrIdx == 0)) {
            return false;
        }
        DerivedPtrPair idxPair = derivePtrTable.GetDerivePair(derivedPtrIdx - 1);
        U32 regIdx = idxPair.first;
        U32 slotIdx = idxPair.second;
        if (regIdx != 0) {
            VisitRegDerivedPtr(visitor, debugVisitor, regSlotsMap, basePtr, regIdx - 1);
        }
        if (slotIdx != 0) {
            VisitSlotDerivedPtr(visitor, debugVisitor, basePtr, fp, slotIdx - 1);
        }
        derivedPtrIdx++;
        return true;
    }

private:
    inline void VisitRegDerivedPtr(const DerivedPtrVisitor& visitor, const DerivedPtrDebugVisitor debugVisitor,
                                   RegSlotsMap& regSlotsMap, BasePtrType basePtr, U32 regIdx) const
    {
        RegRoot regRoot(regTable.GetActiveRegBits(regIdx));
        RootVisitor rootVisitor = [&visitor, basePtr](RootSlot& derivedRoot) {
            visitor(basePtr, DerivedSlotAt(&derivedRoot));
        };
        RegDebugVisitor regDebug = nullptr;
        (void)debugVisitor;
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
        if (debugVisitor != nullptr) {
            regDebug = [&debugVisitor, basePtr](RegisterNum, zaddress_unsafe derivedPtr) {
                debugVisitor(basePtr, derivedPtr);
            };
        }
#endif
        regRoot.VisitGCRoots(rootVisitor, regDebug, regSlotsMap);
    }
    inline void VisitSlotDerivedPtr(const DerivedPtrVisitor& visitor, const DerivedPtrDebugVisitor debugVisitor,
                                    BasePtrType basePtr, Uptr fp, U32 slotIdx) const
    {
        SlotRoot slotRoot(slotTable.GetBaseOffset(slotIdx), slotTable.GetSlotBitMap(slotIdx), slotTable.slotFormat);
        RootVisitor rootVisitor = [&visitor, basePtr](RootSlot& derivedRoot) {
            visitor(basePtr, DerivedSlotAt(&derivedRoot));
        };
        SlotDebugVisitor slotDebug = nullptr;
        (void)debugVisitor;
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
        if (debugVisitor != nullptr) {
            slotDebug = [&debugVisitor, basePtr](SlotBias, zaddress_unsafe derivedPtr) {
                debugVisitor(basePtr, derivedPtr);
            };
        }
#endif
        slotRoot.VisitGCRoots(rootVisitor, slotDebug, fp);
    }
    DerivedPtrTable derivePtrTable;
    RegTable regTable;
    SlotTable slotTable;
    U32 derivedPtrIdx = 0;
};
} // namespace MapleRuntime
#endif // ~MRT_DERIVED_PTR_ROOT_H
