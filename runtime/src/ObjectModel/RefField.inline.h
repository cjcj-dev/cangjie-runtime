// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_INLINE_H
#define MRT_REF_FIELD_INLINE_H

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Verify/B4PersistProbe.h"
#include "ObjectModel/RefField.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
template<bool isAtomic>
void RefField<isAtomic>::SetTargetObject(const BaseObject* obj, std::memory_order order)
{
    RefField<> newField(obj);
    uintptr_t newVal = newField.GetFieldValue();
    RefFieldValue oldVal = fieldVal;
    (void)oldVal;

    // b4persist: last typed write timeline (default off).
    if (B4PersistProbe::Enabled()) {
        B4PersistProbe::NoteTypedWrite(this, const_cast<BaseObject*>(obj), "SetTargetObject",
                                       __builtin_return_address(0), __builtin_return_address(1),
                                       __builtin_return_address(2));
    }

    if (isAtomic) {
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAtomicStore(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#else
        __atomic_store_n(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#endif
    } else {
        fieldVal = static_cast<RefFieldValue>(newVal);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemory(&fieldVal, GetSize());
#endif
    }

    DLOG(BARRIER, "write field @%p 0x%zx -> %p", this, oldVal, obj);
}

template<bool isAtomic>
void RefField<isAtomic>::SetFieldValue(MAddress newVal, std::memory_order order)
{
    RefFieldValue oldVal = fieldVal;
    (void)oldVal;

    if (B4PersistProbe::Enabled()) {
        BaseObject* asObj = reinterpret_cast<BaseObject*>(RefField<>(newVal).GetAddress());
        B4PersistProbe::NoteTypedWrite(this, asObj, "SetFieldValue", __builtin_return_address(0),
                                       __builtin_return_address(1), __builtin_return_address(2));
    }

    if (isAtomic) {
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAtomicStore(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#else
        __atomic_store_n(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#endif
    } else {
        fieldVal = static_cast<RefFieldValue>(newVal);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanWriteMemory(&fieldVal, GetSize());
#endif
    }
    DLOG(BARRIER, "write field @%p 0x%zx -> 0x%zx", this, oldVal, newVal);
}
} // namespace MapleRuntime
#endif // MRT_REF_FIELD_INLINE_H
