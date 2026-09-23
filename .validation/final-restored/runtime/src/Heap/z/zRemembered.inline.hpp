// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_REMEMBERED_INLINE_HPP
#define MRT_Z_REMEMBERED_INLINE_HPP

#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"

namespace MapleRuntime {

inline void ZRemembered::remember(volatile zpointer* p) const
{
    ZPage* page = _page_table->get(reinterpret_cast<MAddress>(p));
    CHECK(page != nullptr);
    page->remember(p);
}

inline bool ZRemembered::is_remembered(volatile zpointer* p) const
{
    ZPage* page = _page_table->get(reinterpret_cast<MAddress>(p));
    CHECK(page != nullptr);
    return page->is_remembered(p);
}

} // namespace MapleRuntime

#endif
