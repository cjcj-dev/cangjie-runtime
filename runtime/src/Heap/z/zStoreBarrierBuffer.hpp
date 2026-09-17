// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STORE_BARRIER_BUFFER_H
#define MRT_STORE_BARRIER_BUFFER_H

#include <cstddef>
#include <mutex>

#include "Common/TypeDef.h"
#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {
class Collector;
constexpr bool kBufferStoreBarriers = true;
constexpr size_t kStoreBarrierBufferLength = 32;

struct StoreBarrierEntry {
    MAddress p = 0;
    zpointer prev = zpointer::null;
};

class StoreBarrierBuffer {
public:
    StoreBarrierBuffer();
    void Initialize(uintptr_t color);

    bool IsEmpty() const;
    size_t Current() const;
    size_t Pending() const { return kStoreBarrierBufferLength - current; }
    static constexpr size_t Capacity() { return kStoreBarrierBufferLength; }
    static StoreBarrierBuffer* buffer_for_store(bool heal);

    void add(MAddress p, zpointer prev);
    void Flush();
    void install_base_pointers();
    void on_new_phase();
    bool is_in(MAddress p) const;

    friend class MutatorManager;
    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};
    size_t current;

private:
    void clear();
    void install_base_pointers_inner();
    void on_new_phase_relocate(size_t i);
    void on_new_phase_remember(size_t i);
    void on_new_phase_mark(size_t i);
    bool is_old_mark() const;
    bool stored_during_old_mark() const;

    uintptr_t lastProcessedColor;
    uintptr_t lastInstalledColor;
    std::mutex basePointerLock;
    zaddress_unsafe basePointers[kStoreBarrierBufferLength] {};
};

} // namespace MapleRuntime

#include "Heap/z/zStoreBarrierBuffer.inline.hpp"
#endif
