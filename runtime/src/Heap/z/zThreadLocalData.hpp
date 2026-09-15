// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstddef>
#include <cstdint>
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkStack.hpp"
namespace MapleRuntime {
// ZThreadLocalData, zThreadLocalData.hpp:35-57. Mark stacks belong to
// the thread data itself; the store buffer has its own allocation/lifetime.
struct ThreadGCData {
    uintptr_t loadGoodMask = 0;
    uintptr_t loadBadMask = 0;
    uintptr_t markBadMask = 0;
    uintptr_t storeGoodMask = 0;
    uintptr_t storeBadMask = 0;
    StoreBarrierBuffer* storeBarrierBuffer;
    MarkThreadLocalStacks markStacks[2];
    zaddress_unsafe* invisibleRoot = nullptr;

    ThreadGCData() : storeBarrierBuffer(new StoreBarrierBuffer()) {}
    ~ThreadGCData() { delete storeBarrierBuffer; }
    ThreadGCData(const ThreadGCData&) = delete;
    ThreadGCData& operator=(const ThreadGCData&) = delete;

    static size_t load_bad_mask_offset() { return offsetof(ThreadGCData, loadBadMask); }
    static size_t mark_bad_mask_offset() { return offsetof(ThreadGCData, markBadMask); }
    static size_t store_bad_mask_offset() { return offsetof(ThreadGCData, storeBadMask); }
    static size_t store_good_mask_offset() { return offsetof(ThreadGCData, storeGoodMask); }
    static size_t store_barrier_buffer_offset() { return offsetof(ThreadGCData, storeBarrierBuffer); }
};

} // namespace MapleRuntime
