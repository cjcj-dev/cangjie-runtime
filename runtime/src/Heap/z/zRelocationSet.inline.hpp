// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_SET_INLINE_H
#define MRT_RELOCATION_SET_INLINE_H

#include "Heap/z/zRelocationSet.hpp"

#include "Heap/z/zArray.inline.hpp"

namespace MapleRuntime {

template <bool Parallel>
inline ZRelocationSetIteratorImpl<Parallel>::ZRelocationSetIteratorImpl()
    : ZArrayIteratorImpl<ZForwarding*, Parallel>(nullptr, 0) {}

template <bool Parallel>
inline ZRelocationSetIteratorImpl<Parallel>::ZRelocationSetIteratorImpl(ZRelocationSet* relocation_set)
    : ZArrayIteratorImpl<ZForwarding*, Parallel>(relocation_set->_forwardings, relocation_set->_nforwardings) {}

} // namespace MapleRuntime

#endif
