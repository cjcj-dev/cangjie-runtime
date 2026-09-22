// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#pragma once
#include "Heap/z/zStoreBarrierBuffer.hpp"
namespace MapleRuntime {
inline void StoreBarrierBuffer::add(MAddress p, zpointer prev)
{
    if (current == 0) {
        Flush();
    }
    current -= sizeof(StoreBarrierEntry);
    buffer[Current()] = { reinterpret_cast<volatile zpointer*>(p), prev };
}

inline size_t StoreBarrierBuffer::Current() const { return current / sizeof(StoreBarrierEntry); }
}
