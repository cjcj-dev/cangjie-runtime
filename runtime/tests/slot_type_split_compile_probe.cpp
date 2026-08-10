// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Negative-compile probe for the three-slot type discipline
// (HeapSlot / RootSlot / DerivedSlot, ObjectModel/RefField.h).
//
// How this file is run: runtime/CMakeLists.txt compiles it once per arm at configure
// time and asserts the outcome, so it runs in every build anyone does. Arms:
//   control  -- no MRT_* macro defined            -> MUST compile
//   witness  -- MRT_EXPLICIT_*                    -> MUST compile (documented bypass)
//   negative -- each MRT_NEGATIVE_* alone         -> MUST fail, and the diagnostic
//               must mention the named symbol (rc!=0 alone does not distinguish
//               "the type stopped it" from "the include path was wrong")
//
// Adding a rule: add the case here plus its row in CMakeLists.txt (arm + expected
// diagnostic token). A case with no row is not checked by anything.

#include <type_traits>

#include "ObjectModel/RefField.h"

using namespace MapleRuntime;

// ── always-on invariants (checked by every arm, including the control) ───────
static_assert(!std::is_convertible<zpointer, zaddress>::value,
              "a coloured pointer must not convert to a plain root value");

// The split is only a split if the three slot types do not silently interconvert.
static_assert(!std::is_convertible<HeapSlot<>*, RootSlot*>::value,
              "a heap field must not pass as a root slot");
static_assert(!std::is_convertible<RootSlot*, HeapSlot<>*>::value,
              "a root slot must not pass as a heap field");
static_assert(!std::is_convertible<RootSlot*, DerivedSlot*>::value,
              "a root slot must not pass as a derived slot");
static_assert(!std::is_convertible<DerivedSlot*, RootSlot*>::value,
              "a derived slot must not pass as a root slot");

#if defined(MRT_NEGATIVE_ROOT_COLOURED_WRITE)
void WrongRootWrite(RootSlot& root, zpointer coloured)
{
    // OpenJDK zUncoloredRoot.inline.hpp:35-60 stores only the load-good address.
    StorePlain(root, coloured);
}
#endif

#if defined(MRT_EXPLICIT_ROOT_COLOUR_BYPASS)
void ExplicitRootColourBypass(RootSlot& root, zpointer coloured)
{
    // Audit witness: public raw construction can deliberately wash the state.
    StorePlain(root, to_zaddress(raw(coloured)));
}
#endif

// ── WriteStaticStruct 收口（reldrylinux / b123ff4f）──────────────────────────
// 静态槽批量写经 GCTib::ForEachRootSlot 交出 RootSlot，所以「对静态槽 CAS 带色」这条
// 写法必须编译失败。⚠ 「StorePlain(slot, coloured)」不再单列 —— 它与上面的
// MRT_NEGATIVE_ROOT_COLOURED_WRITE 是【同一个表达式】，重复一遍不增加分辨力。
#if defined(MRT_NEGATIVE_STATIC_STRUCT_CAS_COLOURED)
void WrongStaticStructCas(RootSlot& slot, zpointer coloured)
{
    // RootSlot 没有 CompareExchange —— 静态槽不是 HeapSlot，且 CAS 坐在 relroroot 的只读页面上。
    slot.CompareExchange(coloured, coloured);
}
#endif

// ── heapdesired（5df0d717）────────────────────────────────────────────────────
// HeapSlot(const BaseObject*) 已私有（friend WCollector）：外部代码不得把 plain 值
// 当堆 CAS 的 desired 递进去。此前这条纪律【没有常驻探针】。
#if defined(MRT_NEGATIVE_HEAP_CAS_PLAIN_DESIRED)
void WrongHeapCasPlainDesired(HeapSlot<>& field, zpointer expected, BaseObject* obj)
{
    field.CompareExchange(expected, RefField<>(obj).GetFieldValue());
}
#endif

// ── DerivedSlot（三类槽的第三类）──────────────────────────────────────────────
// StoreDerived 私有、只有 RebaseDerived 是 friend：内点只能由 (base, offset) 重建，
// ⛔ 不能塞一个裸地址进去（那正是 introot 那个「回溯猜 base」的启发式的入口）。
#if defined(MRT_NEGATIVE_DERIVED_RAW_STORE)
void WrongDerivedRawStore(DerivedSlot& slot, const RootSlot& base)
{
    slot.StoreDerived(base, 8, std::memory_order_relaxed);
}
#endif

// ── ReadOnlyRootSlot（静态/RELRO 只读根）──────────────────────────────────────
// ReadOnlyRootSlot = const RootSlot：读路径能收它，写路径必须收不下
// —— relroroot(B-4 ⑤) 实证这些页可能是 r--p，写进去必 SEGV。
#if defined(MRT_NEGATIVE_READONLY_ROOT_WRITE)
void WrongReadOnlyRootWrite(ReadOnlyRootSlot& root, zaddress plain)
{
    StorePlain(root, plain);
}
#endif
