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
