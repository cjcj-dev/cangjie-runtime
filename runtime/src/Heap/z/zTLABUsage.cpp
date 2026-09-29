// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zTLABUsage.hpp"
#include "Base/LogFile.h"

namespace MapleRuntime {
ZTLABUsage::ZTLABUsage() : _used(0), _used_history() {}

void ZTLABUsage::increase_used(size_t size)
{
    _used.fetch_add(size, std::memory_order_relaxed);
}

void ZTLABUsage::decrease_used(size_t size)
{
    CHECK(size <= _used.load(std::memory_order_relaxed));
    _used.fetch_sub(size, std::memory_order_relaxed);
}

void ZTLABUsage::reset()
{
    const size_t used = _used.exchange(0);
    // ZGC zTLABUsage.cpp:44-47: idle cycles do not enter the history.
    if (used == 0) {
        return;
    }
    const size_t oldUsed = tlab_used();
    const size_t oldCapacity = tlab_capacity();
    _used_history.add(used);
    VLOG(REPORT, "TLAB usage update: used %zu -> %zu, capacity %zu -> %zu",
         oldUsed, tlab_used(), oldCapacity, tlab_capacity());
}

size_t ZTLABUsage::tlab_used() const
{
    return _used_history.last();
}

size_t ZTLABUsage::tlab_capacity() const
{
    return _used_history.davg();
}
}
