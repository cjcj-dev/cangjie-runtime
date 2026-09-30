// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once
#include <cstdint>
#include <atomic>
namespace MapleRuntime {
class BaseObject;
// gc/shared/collectedHeap.hpp:297-317: parsable dummy objects for unused memory.
class CollectedHeap {
public:
    uint32_t total_collections() const { return _total_collections.load(std::memory_order_acquire); }
    void increment_total_collections() { _total_collections.fetch_add(1, std::memory_order_release); }
    static void fill_with_dummy_object(uintptr_t start, uintptr_t end, bool zap);
    static bool is_filler_object(const BaseObject* obj);
private:
    std::atomic<uint32_t> _total_collections{0};
};
}
