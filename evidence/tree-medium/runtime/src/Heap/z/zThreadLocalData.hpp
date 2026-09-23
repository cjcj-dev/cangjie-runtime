// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zThreadLocalDataABI.hpp"
namespace MapleRuntime {
class Mutator;
class ZMark;
struct ThreadLocalData;
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
    ~ThreadGCData();

    struct Masks {
        uintptr_t loadGood;
        uintptr_t loadBad;
        uintptr_t markBad;
        uintptr_t storeGood;
        uintptr_t storeBad;
    };
    static void PublishMasks(const Masks& masks);
    static Masks PublishedMasks();
    void InstallMasks(const Masks& masks);
    void Attach(Mutator* owner, ThreadLocalData* nativeOwner, zaddress_unsafe* root);
    void Detach();
    bool FlushMarkStacks(ZMark& domain);
    static void VisitOwners(const std::function<void(ThreadGCData&, Mutator*, ThreadLocalData*)>& visitor);
    ThreadGCData(const ThreadGCData&) = delete;
    ThreadGCData& operator=(const ThreadGCData&) = delete;

    static constexpr size_t load_bad_mask_offset() { return offsetof(ThreadGCData, loadBadMask); }
    static constexpr size_t mark_bad_mask_offset() { return offsetof(ThreadGCData, markBadMask); }
    static constexpr size_t store_bad_mask_offset() { return offsetof(ThreadGCData, storeBadMask); }
    static constexpr size_t store_good_mask_offset() { return offsetof(ThreadGCData, storeGoodMask); }
    static constexpr size_t store_barrier_buffer_offset() { return offsetof(ThreadGCData, storeBarrierBuffer); }
};

#if UINTPTR_MAX == UINT64_MAX
// A layout change must fail here before a compiler can consume stale offsets.
static_assert(offsetof(ThreadGCData, loadGoodMask) == ThreadGCDataABI::LoadGoodMask,
              "ThreadGCData ABI: loadGoodMask");
static_assert(offsetof(ThreadGCData, loadBadMask) == ThreadGCDataABI::LoadBadMask,
              "ThreadGCData ABI: loadBadMask");
static_assert(offsetof(ThreadGCData, markBadMask) == ThreadGCDataABI::MarkBadMask,
              "ThreadGCData ABI: markBadMask");
static_assert(offsetof(ThreadGCData, storeGoodMask) == ThreadGCDataABI::StoreGoodMask,
              "ThreadGCData ABI: storeGoodMask");
static_assert(offsetof(ThreadGCData, storeBadMask) == ThreadGCDataABI::StoreBadMask,
              "ThreadGCData ABI: storeBadMask");
static_assert(offsetof(ThreadGCData, storeBarrierBuffer) == ThreadGCDataABI::StoreBarrierBuffer,
              "ThreadGCData ABI: storeBarrierBuffer");
#endif

} // namespace MapleRuntime
