// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <type_traits>

#include "ObjectModel/RefField.h"

using namespace MapleRuntime;

static_assert(!std::is_convertible<zpointer, zaddress>::value,
              "a coloured pointer must not convert to a plain root value");

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

// ── WriteStaticStruct 收口（reldrylinux）────────────────────────────────────
// 静态/全局批量写的 post-copy fixup 现在经 GCTib::ForEachRootSlot 交出 RootSlot。
// 以下两种「把带色值写进静态槽」的写法必须【编译失败】。正臂 = 整个 runtime 编得过。

#if defined(MRT_NEGATIVE_STATIC_STRUCT_CAS_COLOURED)
void WrongStaticStructCas(RootSlot& slot, zpointer coloured)
{
    // RootSlot 没有 CompareExchange —— 静态槽不是 HeapSlot，且 CAS 坐在 relroroot 的只读页面上。
    slot.CompareExchange(coloured, coloured);
}
#endif

#if defined(MRT_NEGATIVE_STATIC_STRUCT_STORE_COLOURED)
void WrongStaticStructStore(RootSlot& slot, zpointer coloured)
{
    // StorePlain 只收 zaddress；zpointer 不可转换（本文件顶部的 static_assert 已钉死）。
    StorePlain(slot, coloured);
}
#endif
