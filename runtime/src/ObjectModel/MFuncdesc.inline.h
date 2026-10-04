// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MFUNC_DESC_INLINE_H
#define MRT_MFUNC_DESC_INLINE_H

#include "Common/StackType.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "MFuncdesc.h"

namespace MapleRuntime {
inline Uptr* MFuncDesc::GetStackMap() const { return stackMap.GetDataRef(); }

inline U32 MFuncDesc::GetCodeSize() const { return codeSize; }

inline bool MFuncDesc::HasReturnPoll() const
{
#ifdef __APPLE__
    static_assert(offsetof(MFuncDesc, returnPollFlag) == 32, "compiler layout FuncDescReturnPollOffsetMachO");
#else
    static_assert(offsetof(MFuncDesc, returnPollFlag) == 28, "compiler layout FuncDescReturnPollOffsetELF");
#endif
    return (returnPollFlag & 1u) != 0;
}

inline Uptr MFuncDesc::GetAOTEntry() const
{
#ifdef __APPLE__
    static_assert(offsetof(MFuncDesc, entryRel) == 40, "compiler layout FuncDescEntryOffsetMachO");
    static_assert(sizeof(MFuncDesc) == 56, "compiler layout FuncDescStrideMachO");
#else
    static_assert(offsetof(MFuncDesc, entryRel) == 32, "compiler layout FuncDescEntryOffsetELF");
    static_assert(sizeof(MFuncDesc) == 48, "compiler layout FuncDescStrideELF");
#endif
    return reinterpret_cast<Uptr>(entryRel.GetDataRef());
}

inline U8* MFuncDesc::GetAOTQualification() const
{
#ifdef __APPLE__
    static_assert(offsetof(MFuncDesc, qualificationRel) == 48, "compiler layout FuncDescQualificationOffsetMachO");
    static_assert(offsetof(MFuncDesc, qualificationTag) == 52, "compiler layout FuncDescQualificationTagOffsetMachO");
#else
    static_assert(offsetof(MFuncDesc, qualificationRel) == 36, "compiler layout FuncDescQualificationOffsetELF");
    static_assert(offsetof(MFuncDesc, qualificationTag) == 40, "compiler layout FuncDescQualificationTagOffsetELF");
#endif
    return qualificationRel.GetDataRef();
}

inline bool MFuncDesc::HasAOTQualificationTag() const
{
#ifndef __APPLE__
    static_assert(offsetof(MFuncDesc, qualificationReserved) == 44, "compiler layout FuncDescReservedOffsetELF");
    if (qualificationReserved != 0) { return false; }
#endif
    return qualificationTag == AOT_QUALIFICATION_TAG;
}

inline Uptr* MFuncDesc::GetEHTable() const
{
#ifdef __APPLE__
    return reinterpret_cast<Uptr*>(ehTable.refOffset);
#else
    return ehTable.GetDataRef();
#endif
}

inline CString MFuncDesc::GetFuncName() const { return GetStringFromDict(name); }

inline CString MFuncDesc::GetFuncDir() const { return GetStringFromDict(directory); }

inline CString MFuncDesc::GetFuncFilename() const { return GetStringFromDict(filename); }

inline int8_t MFuncDesc::GetStackTraceFormat() const
{
    Uptr base = reinterpret_cast<Uptr>(this);
    // 1: stack trace format flag size, 1 bytes
    return *(reinterpret_cast<const int8_t*>(dictOffsets + base - 1));
}

inline FuncDescRef MFuncDesc::GetFuncDesc(FrameAddress* fa)
{
    ElfUnloadQuiescence::ReadScope reader;
    const Uptr startPC = reinterpret_cast<Uptr>(FrameInfo::GetFuncStartPCFromFrameAddress(fa));
    const auto image = ElfUnloadQuiescence::RegisteredImageForAddress(startPC, true);
    if (image == nullptr) { return nullptr; }
    FuncDescRef desc = reinterpret_cast<FuncDescRef>(
        *reinterpret_cast<U64*>(reinterpret_cast<uintptr_t>(fa) - STACK_OFFSET_IN_APPLE));
    return image->ContainsFunctionDescriptor(reinterpret_cast<Uptr>(desc)) ? desc : nullptr;
}

inline FuncDescRef MFuncDesc::GetFuncDesc(Uptr startPC)
{
    ElfUnloadQuiescence::ReadScope reader;
#ifdef __APPLE__
    return reinterpret_cast<FuncDescRef>(ElfUnloadQuiescence::FindFunctionDescriptor(startPC));
#else
    const auto image = ElfUnloadQuiescence::RegisteredImageForAddress(startPC, true);
    if (image == nullptr) { return nullptr; }
    if (startPC < START_PC_OFFSET) { return nullptr; }
    // A prefix may cross adjacent LOAD ranges of this registration. Check
    // every byte before reading the offset; matching endpoints can hide a gap.
    for (Uptr byte = startPC - START_PC_OFFSET; byte < startPC; ++byte) {
        if (!image->Contains(byte)) { return nullptr; }
    }
    DataRefOffset32<MFuncDesc>* offset =
        reinterpret_cast<DataRefOffset32<MFuncDesc>*>(startPC - START_PC_OFFSET);
    FuncDescRef desc = offset->GetDataRef();
    return image->ContainsFunctionDescriptor(reinterpret_cast<Uptr>(desc)) ? desc : nullptr;
#endif
}
} // namespace MapleRuntime
#endif // MRT_MFUNC_DESC_INLINE_H
