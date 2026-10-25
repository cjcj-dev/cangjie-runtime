// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MFUNC_DESC_H
#define MRT_MFUNC_DESC_H

#include "Common/Dataref.h"
#include "Common/StackType.h"

namespace MapleRuntime {
class MFuncDesc {
public:
    inline Uptr* GetStackMap() const;
    inline U32 GetCodeSize() const;
    inline bool HasReturnPoll() const;
    inline Uptr GetAOTEntry() const;
    inline U8* GetAOTQualification() const;
    inline bool HasAOTQualificationTag() const;
    static constexpr U32 AOT_QUALIFICATION_TAG = 0x31514a43;
    inline Uptr* GetEHTable() const;
    inline CString GetFuncName() const;
    inline CString GetFuncDir() const;
    inline CString GetFuncFilename() const;
    inline int8_t GetStackTraceFormat() const;
    CString GetStringFromDict(U32 offset) const;

    static FuncDescRef GetFuncDesc(Uptr startPC);

private:
    DataRefOffset32<Uptr> stackMap;
    U32 codeSize;
    U32 name;
    U32 directory;
    U32 filename;
    U32 dictOffsets;
#ifdef __APPLE__
    DataRefOffset64<Uptr> ehTable;
#else
    DataRefOffset32<Uptr> ehTable;
#endif

    U32 returnPollFlag;
#ifdef __APPLE__
    DataRefOffset64<U8> entryRel;
#else
    DataRefOffset32<U8> entryRel;
#endif
    DataRefOffset32<U8> qualificationRel;
    U32 qualificationTag;
#ifndef __APPLE__
    U32 qualificationReserved;
#endif

    DISABLE_CLASS_IMPLICIT_CONSTRUCTORS(MFuncDesc);
    DISABLE_CLASS_IMPLICIT_DESTRUCTION(MFuncDesc);

    static constexpr U32 STACK_OFFSET_IN_APPLE = 16u;
    static constexpr uint32_t START_PC_OFFSET = 4u;
};
} // namespace MapleRuntime
#endif // MRT_MFUNC_DESC_H
