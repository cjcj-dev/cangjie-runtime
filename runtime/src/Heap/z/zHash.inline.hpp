// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zHash.inline.hpp:56-81 (hash body (C) 2009 Remo Dentato, BSD; see the
// notice in the reference file).
#pragma once
#include "Heap/z/zHash.hpp"

#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {
inline uint32_t ZHash::uint32_to_uint32(uint32_t key)
{
    key = ~key + (key << 15);
    key = key ^ (key >> 12);
    key = key + (key << 2);
    key = key ^ (key >> 4);
    key = key * 2057;
    key = key ^ (key >> 16);
    return key;
}

// zHash.inline.hpp:73-79: the only ZGC consumer is ZNMethodTable::first_index
// (I1, PLAN §5: no nmethod table); kept so the unit has ZGC's full surface.
inline uint32_t ZHash::address_to_uint32(uintptr_t key)
{
    return uint32_to_uint32((uint32_t)(key >> 3));
}

inline uint32_t ZHash::offset_to_uint32(zoffset key)
{
    return address_to_uint32(untype(key));
}
} // namespace MapleRuntime
