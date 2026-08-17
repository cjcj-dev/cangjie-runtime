// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_INLINE_H
#define MRT_REF_FIELD_INLINE_H

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "ObjectModel/RefField.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
// AssertColouredWriteIfEnabled: single definition in BaseObject.cpp (avoids weak multi-static).

template<bool isAtomic>
void HeapSlot<isAtomic>::StoreColoured(zpointer value, std::memory_order order)
{
    MAddress newVal = raw(value);
    AssertColouredWriteIfEnabled(this, newVal);
#if defined(CANGJIE_TSAN_SUPPORT)
    RefFieldValue oldVal = static_cast<RefFieldValue>(Sanitizer::TsanAtomicLoad(&fieldVal, std::memory_order_relaxed));
#else
    RefFieldValue oldVal = __atomic_load_n(&fieldVal, std::memory_order_relaxed);
#endif
    (void)oldVal;

#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAtomicStore(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#else
    __atomic_store_n(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#endif
    DLOG(BARRIER, "write field @%p 0x%zx -> 0x%zx", this, oldVal, newVal);
}
} // namespace MapleRuntime
#endif // MRT_REF_FIELD_INLINE_H
