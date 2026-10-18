// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include <cstdint>
#include <atomic>
#include "Base/Panic.h"
#include "Common/Mutex.h"
namespace MapleRuntime {
class BaseObject;
// gc/shared/collectedHeap.hpp:297-317: parsable dummy objects for unused memory.
class CollectedHeap {
public:
    uint32_t total_collections() const { return _total_collections.load(std::memory_order_acquire); }
    uint32_t total_full_collections() const { return _total_full_collections.load(std::memory_order_acquire); }
    void increment_total_collections(bool full = false)
    {
        _total_collections.fetch_add(1, std::memory_order_release);
        if (full) { _total_full_collections.fetch_add(1, std::memory_order_release); }
    }
    bool is_shutting_down() const
    {
        CHECK_DETAIL(HeapMonitor().owned_by_self(), "heap shutdown read requires Heap_lock");
        return shuttingDown;
    }
    void initiate_shutdown()
    {
        MonitorLocker lock(HeapMonitor());
        shuttingDown = true;
    }
    static void fill_with_dummy_object(uintptr_t start, uintptr_t end, bool zap);
    static bool is_filler_object(const BaseObject* obj);
private:
    std::atomic<uint32_t> _total_collections{0};
    std::atomic<uint32_t> _total_full_collections{0};
    bool shuttingDown = false;
};
}
