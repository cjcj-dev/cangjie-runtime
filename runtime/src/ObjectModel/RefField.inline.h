// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_INLINE_H
#define MRT_REF_FIELD_INLINE_H

#include <atomic>
#include <cstdio>

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
namespace {
// s3writer: sink-level store probe. Safe: never call methods on tip (may be garbage).
// Logs only clear interiors: tip null / unaligned / small integer (Node.id or Array.length).
inline void LogInteriorRefSink(const void* slot, const BaseObject* obj, MAddress rawVal, const char* kind)
{
    if (obj == nullptr) {
        return;
    }
    // tip = first word at obj; do not invoke TypeInfo methods.
    void* tip = *reinterpret_cast<void* const*>(obj);
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    bool clearInterior = tip == nullptr || (tipAddr & 7) != 0 || tipAddr < 0x10000;
    if (!clearInterior) {
        return;
    }
    static std::atomic<size_t> g_n{ 0 };
    size_t n = g_n.fetch_add(1, std::memory_order_relaxed);
    if (n >= 64) {
        return;
    }
    void* ra0 = __builtin_return_address(0);
    void* ra1 = __builtin_return_address(1);
    void* ra2 = __builtin_return_address(2);
    std::fprintf(stderr,
                 "[GCV2][S3_SET] n=%zu kind=%s slot=%p ref=%p raw=%#zx tip=%p ra0=%p ra1=%p ra2=%p\n", n, kind, slot,
                 obj, static_cast<size_t>(rawVal), tip, ra0, ra1, ra2);
    std::fflush(stderr);
}
} // namespace

template<bool isAtomic>
void RefField<isAtomic>::SetTargetObject(const BaseObject* obj, std::memory_order order)
{
    RefField<> newField(obj);
    uintptr_t newVal = newField.GetFieldValue();
    LogInteriorRefSink(this, obj, static_cast<MAddress>(newVal), "SetTargetObject");
    RefFieldValue oldVal = fieldVal;
    (void)oldVal;

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
    BaseObject* asObj = reinterpret_cast<BaseObject*>(RefField<>(newVal).GetAddress());
    LogInteriorRefSink(this, asObj, newVal, "SetFieldValue");
    RefFieldValue oldVal = fieldVal;
    (void)oldVal;

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
