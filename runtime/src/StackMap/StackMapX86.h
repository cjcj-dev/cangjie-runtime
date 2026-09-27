// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_STACKMAP_X86_64_H
#define MRT_STACKMAP_X86_64_H
#include <vector>

#include "Common/RegisterX86-64.h"
#include "Common/X86StubLayout.h"
#include "StackMap/StackMapTypeDef.h"

namespace MapleRuntime {
static_assert(XMM15 < sizeof(RegBits) * 8, "all x86 register bits must be representable");

class RegRoot {
public:
    RegRoot() = default;
    explicit RegRoot(RegBits bits) : regBits(bits) {}
    ~RegRoot() = default;

    RegRoot(const RegRoot& other) : regBits(other.regBits) {}
    RegRoot(RegRoot&& other) : regBits(other.regBits) { other.regBits = 0; }
    RegRoot& operator=(const RegRoot& other)
    {
        if (this == &other) {
            return *this;
        }
        regBits = other.regBits;
        return *this;
    }
    RegRoot& operator=(RegRoot&& other)
    {
        if (this == &other) {
            return *this;
        }
        regBits = other.regBits;
        other.regBits = 0;
        return *this;
    }

    static void RecordStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
    {
        regSlotsMap.allRegistersSaved = false;
        constexpr Uptr slotLength = 8;
        Uptr slotAddr = fp - slotLength;
#ifdef _WIN64
        static constexpr RegisterId calleeSavedRegister[] = { R15, R14, R13, R12, RDI, RSI, RBX };
#else
        static constexpr RegisterId calleeSavedRegister[] = { R15, R14, R13, R12, RBX };
#endif
        for (auto reg : calleeSavedRegister) {
            regSlotsMap.Insert(reg, &RootSlotAt(slotAddr));
            slotAddr -= slotLength;
        }
    }

#ifdef _WIN64
    static void RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
    {
        regSlotsMap.allRegistersSaved = true;
        constexpr Uptr universalSlotLength = 8;
        constexpr Uptr xmmSlotLength = 16;
        constexpr U32 stubPushNum = 14;
        constexpr RegisterId stubPushOrder[stubPushNum] = { RAX, RBX, RCX, RDX, RDI, RSI, R8,
                                                            R9,  R10, R11, R12, R13, R14, R15 };
        Uptr slotAddr = fp;
        for (U32 i = 0; i < stubPushNum; ++i) {
            slotAddr -= universalSlotLength;
            regSlotsMap.Insert(stubPushOrder[i], &RootSlotAt(slotAddr));
        }

        for (U8 i = XMM0; i <= XMM15; ++i) {
            slotAddr -= xmmSlotLength;
            regSlotsMap.Insert(i, &RootSlotAt(slotAddr));
        }
    }
#else
    static void RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
    {
        regSlotsMap.allRegistersSaved = true;
#define RECORD_GPR(reg, id, off) regSlotsMap.Insert(id, &RootSlotAt(fp + (off)));
        MRT_X86_STUB_GPRS(RECORD_GPR)
#undef RECORD_GPR
#define RECORD_XMM(reg, id, off) regSlotsMap.Insert(id, &RootSlotAt(fp + MRT_X86_STUB_XMM_BASE + (off)));
        MRT_X86_STUB_XMMS(RECORD_XMM)
#undef RECORD_XMM
    }
#endif

    bool VisitGCRoots(const RootVisitor& visitor, const RegDebugVisitor& debugFunc, RegSlotsMap& regSlotsMap,
                      std::list<BasePtrType>* rootsList = nullptr) const
    {
        RegBits bits = regBits;
        // Visit universal register roots
        for (RegisterNum i = RAX; i <= R15; ++i, bits >>= 1) {
            if (bits == 0) {
                return true;
            }
            if ((bits & LOWEST_BIT) == 0) {
                continue;
            }
            if (!regSlotsMap.VisitSingleSlotsRoot(visitor, debugFunc, i, rootsList)) {
                return false;
            }
        }
        // RIP occupies bit 16 in the emitter's X86Bit2Reg table.
        bits >>= (XMM0 - R15 - 1);
        // Visit xmm register roots.
        for (RegisterNum i = XMM0; i <= XMM15; ++i, bits >>= 1) {
            if (bits == 0) {
                return true;
            }
            if ((bits & LOWEST_BIT) == 0) {
                continue;
            }
            if (!regSlotsMap.VisitDoubleSlotsRoot(visitor, debugFunc, i)) {
                return false;
            }
        }
        return true;
    }

    size_t CountRootSlots() const
    {
        RegBits bits = regBits;
        size_t count = 0;
        for (RegisterNum i = RAX; i <= R15; ++i, bits >>= 1) {
            if ((bits & LOWEST_BIT) != 0) {
                ++count;
            }
        }
        bits >>= (XMM0 - R15 - 1);
        for (RegisterNum i = XMM0; i <= XMM15; ++i, bits >>= 1) {
            if ((bits & LOWEST_BIT) != 0) {
                count += 2;
            }
        }
        return count;
    }

    static void RecordRegs(RegSlotsMap& regSlotsMap, Uptr fp)
    {
        RecordStubAllRegister(regSlotsMap, fp);
    }
private:
    static constexpr RegBits LOWEST_BIT = 0x1;
    RegBits regBits{ 0 };
};
} // namespace MapleRuntime
#endif // MRT_STACKMAP_X86_64_H
