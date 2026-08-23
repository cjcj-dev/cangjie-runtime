// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STORE_BARRIER_BUFFER_H
#define MRT_STORE_BARRIER_BUFFER_H

#include <cstddef>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class RememberedSet;

// Compile-time switch (ZGC ZBufferStoreBarriers). No MRT_GCV2_* env var.
constexpr bool kBufferStoreBarriers = true;
constexpr size_t kStoreBarrierBufferLength = 32;

// ZGC ZStoreBarrierEntry is (p, prev): heap_store_slow_path buffers SATB mark of the
// overwritten pointer plus remember(p) (zBarrier.cpp:253-261, flush at
// zStoreBarrierBuffer.cpp:278-282). We only defer RememberedSet::Record (bitmap
// fetch_or). Slot address is enough; prev is unused here.
struct StoreBarrierEntry {
    MAddress p = 0;
};

class StoreBarrierBuffer {
public:
    StoreBarrierBuffer() : current(kBufferStoreBarriers ? kStoreBarrierBufferLength : 0) {}

    bool IsEmpty() const { return current == kStoreBarrierBufferLength; }
    size_t Pending() const { return kStoreBarrierBufferLength - current; }
    size_t Current() const { return current; }

    void Add(MAddress fieldAddress, RememberedSet& rs);
    void Flush(RememberedSet& rs);
    // Test-only: drop pending without Record. Used to prove Flush-before-Drain.
    void Discard();

    static void FlushAll(RememberedSet& rs);

private:
    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};
    size_t current;
};
} // namespace MapleRuntime

#endif
