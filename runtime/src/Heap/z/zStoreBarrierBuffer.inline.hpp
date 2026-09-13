// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zStoreBarrierBuffer.hpp"
namespace MapleRuntime {
void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs)
{
    Add(fieldAddress, fieldBase, zpointer::null, rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, nullptr, prev, rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev, RememberedSet& rs)
{
    // One per-thread buffer and one processed color, as in ZStoreBarrierBuffer.
    // Consume an earlier phase before appending entries from the new phase.
    if (current == 0 || lastProcessedColor != static_cast<uintptr_t>(::g_cjStoreGoodMask)) {
        Flush(rs);
    }
    CHECK_DETAIL(fieldBase == nullptr || fieldAddress >= reinterpret_cast<MAddress>(fieldBase),
                 "store-buffer field precedes holder slot=%#zx holder=%p", fieldAddress, fieldBase);
    --current;
    buffer[current] = { fieldAddress, fieldBase,
        fieldBase == nullptr ? 0 : fieldAddress - reinterpret_cast<MAddress>(fieldBase), prev };
}

}

namespace MapleRuntime {
size_t StoreBarrierBuffer::Current() const { return current; }
}
