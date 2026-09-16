// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STORE_BARRIER_BUFFER_H
#define MRT_STORE_BARRIER_BUFFER_H

#include <cstddef>
#include <functional>
#include <mutex>

#include "Common/TypeDef.h"
#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {
class Collector;
class RememberedSet;

constexpr bool kBufferStoreBarriers = true;
constexpr size_t kStoreBarrierBufferLength = 32;

struct StoreBarrierEntry {
    MAddress p = 0;
    zpointer prev = zpointer::null;
};

#if defined(MRT_GC_UNIT_TESTS)
enum class StoreBarrierFlushEvent : uint8_t {
    PREVIOUS_RETIRED,
    PREVIOUS_INVALID,
    SLOT_REMEMBERED,
};
using StoreBarrierFlushObserver = void (*)(StoreBarrierFlushEvent, const StoreBarrierEntry&);
#endif

class StoreBarrierBuffer {
public:
    StoreBarrierBuffer();
    void Initialize(uintptr_t color);

    bool IsEmpty() const;
    size_t Pending() const { return kStoreBarrierBufferLength - current; }
    size_t Current() const;
    void VisitEntries(const std::function<void(const StoreBarrierEntry&)>& visitor) const
    {
        for (size_t i = current; i < kStoreBarrierBufferLength; ++i) { visitor(buffer[i]); }
    }

    static constexpr size_t Capacity() { return kStoreBarrierBufferLength; }
    static StoreBarrierBuffer* buffer_for_store(bool heal);

    void Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs);
    void Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs);
    void Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev, RememberedSet& rs);
    void Flush(RememberedSet& rs);
    void Flush(RememberedSet& rs, Collector& collector);
    void Discard();
    void install_base_pointers();
    void on_new_phase();
    bool is_in(MAddress p) const;

#if defined(MRT_TESTABLE_INTERNALS)
    uintptr_t LastProcessedColorForTest() const;
#endif
#if defined(MRT_GC_UNIT_TESTS)
    static void SetFlushObserverForTest(StoreBarrierFlushObserver observer);
    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};
    size_t current;
#endif

private:
    void clear();
    void install_base_pointers_inner();
    void on_new_phase_relocate(size_t i);
    void on_new_phase_remember(size_t i);
    void on_new_phase_mark(size_t i);
    bool is_old_mark() const;
    bool stored_during_old_mark() const;

#if !defined(MRT_GC_UNIT_TESTS)
    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};
    size_t current;
#endif
    uintptr_t lastProcessedColor;
    uintptr_t lastInstalledColor;
    std::mutex basePointerLock;
    zaddress_unsafe basePointers[kStoreBarrierBufferLength] {};
};

} // namespace MapleRuntime

#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/z/zStoreBarrierBuffer.inline.hpp"
#endif
