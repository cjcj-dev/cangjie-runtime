// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zStoreBarrierBuffer.hpp"
namespace MapleRuntime {
inline void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs)
{
    Add(fieldAddress, fieldBase, zpointer::null, rs);
}

inline void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, nullptr, prev, rs);
}

inline void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject*, zpointer prev, RememberedSet& rs)
{
    if (current == 0) {
        Flush(rs);
    }
    --current;
    buffer[current] = { fieldAddress, prev };
}

inline size_t StoreBarrierBuffer::Current() const { return current; }
}
