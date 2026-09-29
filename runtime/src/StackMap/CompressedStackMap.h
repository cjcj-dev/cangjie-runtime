// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COMPRESSED_STACKMAP_H
#define MRT_COMPRESSED_STACKMAP_H
#include <new>
#include "Common/Dataref.h"
#include "Common/StackType.h"
#include "Common/TypeDef.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "StackMap/DerivedPtr.h"
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
enum class StackMapInvalidReason : U8 {
    NONE,
    NO_MAP,
    ZERO_ENTRIES,
    PC_MISS,
    ZERO_ROOT_INDICES,
};

class CompressedStackMapEntry {
public:
    CompressedStackMapEntry(const IdxSet& idx, const RegTable& reg, const SlotTable& slot, const LineNumTable& lineNum,
                            const DerivedPtrTable derived, U32 derivedRows, bool valid)
        : idxSet(idx), regTable(reg), slotTable(slot),
          lineNumTable(lineNum), derivedPtrTable(derived), derivedPtrRows(derivedRows), isValid(valid) {}
    explicit CompressedStackMapEntry(bool valid) : isValid(valid) {}

    ~CompressedStackMapEntry() = default;

    bool IsValid() const { return isValid; }
    SlotRoot BuildSlotRoot() const
    {
        U32 idx = idxSet.slotIdx;
        if (idx == 0) {
            return SlotRoot();
        }
        return SlotRoot(slotTable.GetBaseOffset(idx - 1), slotTable.GetSlotBitMap(idx - 1), slotTable.slotFormat);
    }

    RegRoot BuildRegRoot() const
    {
        U32 idx = idxSet.regIdx;
        if (idx == 0) {
            return RegRoot();
        }
        return RegRoot(regTable.GetActiveRegBits(idx - 1));
    }

    DerivedPtr BuildDerivedPtrRoot() const
    {
        U32 idx = idxSet.derivedPtrIdx;
        if (idx == 0) {
            return DerivedPtr();
        }
        return DerivedPtr(derivedPtrTable, regTable, slotTable, idx, derivedPtrRows);
    }

    LineNum BuildLineNum() const
    {
        U32 idx = idxSet.lineNumIdx;
        if (idx == 0) {
            return 0;
        }
        return lineNumTable.GetLineNumber(idx - 1);
    }

    SlotRoot BuildStackSlotRoot() const
    {
        U32 idx = idxSet.stackSlotIdx;
        if (idx == 0) {
            return SlotRoot();
        }
        return SlotRoot(slotTable.GetBaseOffset(idx - 1), slotTable.GetSlotBitMap(idx - 1), slotTable.slotFormat);
    }

    SlotRoot BuildOopSlotRoot() const
    {
        U32 idx = idxSet.oopSlotIdx;
        if (idx == 0) {
            return SlotRoot();
        }
        return SlotRoot(slotTable.GetBaseOffset(idx - 1), slotTable.GetSlotBitMap(idx - 1), slotTable.slotFormat);
    }

    RegRoot BuildOopRegRoot() const
    {
        U32 idx = idxSet.oopRegIdx;
        if (idx == 0) {
            return RegRoot();
        }
        return RegRoot(regTable.GetActiveRegBits(idx - 1));
    }

    RegRoot BuildStackRegRoot() const
    {
        U32 idx = idxSet.stackRegIdx;
        if (idx == 0) {
            return RegRoot();
        }
        return RegRoot(regTable.GetActiveRegBits(idx - 1));
    }

private:
    IdxSet idxSet;
    RegTable regTable;
    SlotTable slotTable;
    LineNumTable lineNumTable;
    DerivedPtrTable derivedPtrTable;
    U32 derivedPtrRows{ 0 };
    bool isValid = false;
};
class CompressedStackMapHead {
public:
    CompressedStackMapHead() : table(nullptr) {}
    explicit CompressedStackMapHead(const Uptr* table) : table(table), prologue(table) {}
    CompressedStackMapHead(CompressedStackMapHead&& other) : table(other.table)
    {
        if (table != nullptr) { new (&prologue) FramePrologue(std::move(other.prologue)); }
    }
    ~CompressedStackMapHead()
    {
        if (table != nullptr) { prologue.~FramePrologue(); }
    }
    PrologueRegisterClosure TakePrologueRegisters()
    {
        if (table == nullptr) { return PrologueRegisterClosure(); }
        return prologue.TakeRegisters();
    }
    static CompressedStackMapHead GetStackMapHead(Uptr addr, uint64_t* funcDesc = nullptr)
    {
        ElfUnloadQuiescence::ReadScope metadataReader;
        U8 *stackmapStart = nullptr;
        if (funcDesc)
            stackmapStart = reinterpret_cast<U8*>(reinterpret_cast<FuncDescRef>(funcDesc)->GetStackMap());
        else {
#if defined(__APPLE__)
            FuncDescRef desc = MFuncDesc::GetFuncDesc(reinterpret_cast<FrameAddress*>(addr));
#else
            FuncDescRef desc = MFuncDesc::GetFuncDesc(addr);
#endif
            stackmapStart = reinterpret_cast<U8*>(desc->GetStackMap());
        }
        // A compiled frame may have no oop map (HotSpot frame.cpp:998).
        // Keep absence as a null metadata pointer, never a synthetic prologue.
        return stackmapStart == nullptr ? CompressedStackMapHead() :
            CompressedStackMapHead(reinterpret_cast<Uptr*>(stackmapStart));
    }
    static void DestroyStackMapHead(CompressedStackMapHead*& stackMapHead) noexcept
    {
        if (stackMapHead != nullptr) {
            delete stackMapHead;
            stackMapHead = nullptr;
        }
    }

    CompressedStackMapEntry GetStackMapEntry(Uptr startPC, Uptr framePC, bool countDerivedRows = false) const
    {
        if (table == nullptr) { return CompressedStackMapEntry(false); }
        StackMapTable stackMapTable(prologue.GetNextTable(), prologue.GetSlotFormat());
        if (stackMapTable.GetLookupResult(startPC, framePC) != StackMapLookupResult::FOUND) {
            return CompressedStackMapEntry(false);
        }
        auto idxSet = stackMapTable.GetIdxSet(startPC, framePC);
        RegTable regTable(stackMapTable.GetNextTable());
        SlotTable slotTable(regTable.GetNextTable(), prologue.GetSlotFormat());
        LineNumTable lineTable(slotTable.GetNextTable());
        DerivedPtrTable derivedTable(lineTable.GetNextTable(), stackMapTable.GetRegBitsLen(),
                                     stackMapTable.GetSlotBitsLen());
        U32 derivedRows = countDerivedRows
            ? stackMapTable.GetDerivedPtrRows(startPC, framePC, derivedTable.GetRecordNum())
            : 0;
        return CompressedStackMapEntry(idxSet, regTable, slotTable, lineTable, derivedTable, derivedRows, true);
    }

    StackMapInvalidReason GetInvalidReason(Uptr startPC, Uptr framePC) const
    {
        if (table == nullptr) { return StackMapInvalidReason::NO_MAP; }
        StackMapTable stackMapTable(prologue.GetNextTable(), prologue.GetSlotFormat());
        switch (stackMapTable.GetLookupResult(startPC, framePC)) {
            case StackMapLookupResult::ZERO_ENTRIES:
                return StackMapInvalidReason::ZERO_ENTRIES;
            case StackMapLookupResult::PC_MISS:
                return StackMapInvalidReason::PC_MISS;
            case StackMapLookupResult::FOUND:
                return StackMapInvalidReason::ZERO_ROOT_INDICES;
        }
        return StackMapInvalidReason::ZERO_ROOT_INDICES;
    }

private:
    const Uptr* table;
    // C++14 inline optional storage: absent maps do not construct a decoder.
    // Keep MethodMap's allocation-free path and decode present prologues once.
    union {
        FramePrologue prologue;
    };
};
using StackMapEntry = CompressedStackMapEntry;
using StackMapHead = CompressedStackMapHead;
} // namespace MapleRuntime
#endif // ~MRT_COMPRESSED_STACKMAP_H
