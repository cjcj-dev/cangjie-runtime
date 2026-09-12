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
class Collector;
class RememberedSet;

// Compile-time switch (ZGC ZBufferStoreBarriers). No MRT_GCV2_* env var.
constexpr bool kBufferStoreBarriers = true;
constexpr size_t kStoreBarrierBufferLength = 32;

// ZGC ZStoreBarrierEntry is (p, prev), with a parallel _base_pointers array.
// Before relocation destroys the page liveness map, ZRelocate installs the base
// object for every pending p; a phase flush then relocates the base and rebuilds
// p at the same field offset (zStoreBarrierBuffer.cpp:52-102,130-153;
// zRelocate.cpp:1048-1080,1289-1296).  Keeping only p is not sufficient: a
// pending entry may still name a from-space holder when it is consumed.
struct StoreBarrierEntry {
    MAddress p = 0;
    BaseObject* pBase = nullptr;
    size_t pOffset = 0;
    zpointer prev = zpointer::null;

    MAddress Remap(BaseObject* remappedBase) const
    {
        return remappedBase == nullptr ? p : reinterpret_cast<MAddress>(remappedBase) + pOffset;
    }
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

    bool IsEmpty() const { return current == kStoreBarrierBufferLength; }
    size_t Pending() const { return kStoreBarrierBufferLength - current; }
    size_t Current() const { return current; }
    static constexpr size_t Capacity() { return kStoreBarrierBufferLength; }

    void Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs);
    void Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs);
    void Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev, RememberedSet& rs);
    void Flush(RememberedSet& rs);
    // Test-only: drop pending without Record. Used to prove Flush-before-Drain.
    void Discard();

    static void FlushAll(RememberedSet& rs);
#if defined(MRT_TESTABLE_INTERNALS)
    uintptr_t LastProcessedColorForTest() const { return lastProcessedColor; }
#endif
#if defined(MRT_GC_UNIT_TESTS)
    static void SetFlushObserverForTest(StoreBarrierFlushObserver observer);
#endif

private:
    void Flush(RememberedSet& rs, Collector& collector);
    void MarkAndRemember(const StoreBarrierEntry& entry, RememberedSet& rs, Collector& collector,
                         bool phaseChanged);

    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};
    size_t current;
    uintptr_t lastProcessedColor;
};
} // namespace MapleRuntime

#endif
