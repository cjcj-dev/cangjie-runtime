// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_INLINE_H
#define MRT_REF_FIELD_INLINE_H

#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Common/ColourMask.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
// ops/design/COLOUR_WRITEBACK_AUDIT.md §六 判据 1：堆内 RefField 落笔点拒绝 plain 非 null。
// 默认关；gate 开 MRT_GCV2_ASSERT_COLOURED_WRITES=1。关闭时只读一次 static bool。
// 定义放 inline 头：所有写路径 TU 都 include 本文件；CompareExchange 在头内调用
// 同名声明，靠本定义 ODR 合并。
inline void AssertColouredWriteIfEnabled(const void* slot, MAddress newVal)
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_ASSERT_COLOURED_WRITES");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    if (LIKELY(!on)) {
        return;
    }
    // 静态槽不在堆地址面：本断言覆盖不到，靠 plainHeapRefSlots 扫描器（批后续）。
    if (!Heap::IsHeapAddress(slot)) {
        return;
    }
    // null 合法 plain（审计 Y3）：读者先判 payload。
    if ((newVal & ((MAddress(1) << 48) - 1)) == 0) {
        return;
    }
    constexpr MAddress kColourMask = REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK;
    bool hasColour = (newVal & kColourMask) != 0;
    bool tagged = ((newVal >> 48) & 1) != 0;
    bool loadGood = (newVal & static_cast<MAddress>(::g_cjLoadBadMask)) == 0;
    CHECK_DETAIL(hasColour && (loadGood || tagged),
                 "MRT_GCV2_ASSERT_COLOURED_WRITES: plain/bad-colour heap ref write @%p val=%#zx "
                 "hasColour=%d loadGood=%d tagged=%d",
                 slot, newVal, hasColour, loadGood, tagged);
}

template<bool isAtomic>
void RefField<isAtomic>::SetTargetObject(const BaseObject* obj, std::memory_order order)
{
    RefField<> newField(obj);
    uintptr_t newVal = newField.GetFieldValue();
    AssertColouredWriteIfEnabled(this, static_cast<MAddress>(newVal));
#if defined(CANGJIE_TSAN_SUPPORT)
    RefFieldValue oldVal = static_cast<RefFieldValue>(Sanitizer::TsanAtomicLoad(&fieldVal, std::memory_order_relaxed));
#else
    RefFieldValue oldVal = __atomic_load_n(&fieldVal, std::memory_order_relaxed);
#endif
    (void)oldVal;

    // Always atomic: see GetTargetObject — closes mutator↔GC races R1/R3.
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAtomicStore(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#else
    __atomic_store_n(&fieldVal, static_cast<RefFieldValue>(newVal), order);
#endif

    DLOG(BARRIER, "write field @%p 0x%zx -> %p", this, oldVal, obj);
}

template<bool isAtomic>
void RefField<isAtomic>::SetFieldValue(MAddress newVal, std::memory_order order)
{
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
