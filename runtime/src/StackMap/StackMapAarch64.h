// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_STACKMAP_AARCH64_H
#define MRT_STACKMAP_AARCH64_H
#include <stdint.h>
#include <unordered_map>

#include "StackMap/StackMapTypeDef.h"
#include "Common/Aarch64StubLayout.h"

namespace MapleRuntime {
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
#define RECORD_CALLEE(reg, id, index, off) regSlotsMap.Insert(id, &RootSlotAt(fp + off));
        MRT_AARCH64_STUB_CALLEE_GPRS(RECORD_CALLEE)
#undef RECORD_CALLEE
    }

    static void RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
    {
        regSlotsMap.allRegistersSaved = true;
#define RECORD_ALL(reg, id, index, off) \
        if (id <= X28) { regSlotsMap.Insert(id, &RootSlotAt(fp + off)); }
        MRT_AARCH64_STUB_GPRS(RECORD_ALL)
#undef RECORD_ALL
    }

    bool VisitGCRoots(const RootVisitor& visitor, const RegDebugVisitor& debugFunc, const RegSlotsMap& regSlotsMap,
                      std::list<BasePtrType>* rootsList = nullptr) const
    {
        RegBits bits = regBits;
        for (RegisterNum i = 0; bits != 0; ++i, bits >>= 1) {
            if ((bits & LOWEST_BIT) == 0) {
                continue;
            }
            if (!regSlotsMap.VisitSingleSlotsRoot(visitor, debugFunc, i, rootsList)) {
                return false;
            }
        }
        return true;
    }

    size_t CountRootSlots() const
    {
        RegBits bits = regBits;
        size_t count = 0;
        while (bits != 0) {
            count += bits & LOWEST_BIT;
            bits >>= 1;
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
#endif // MRT_STACKMAP_AARCH64_H
