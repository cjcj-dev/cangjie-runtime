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

// ZGC ZStoreBarrierEntry keeps the field and the overwritten coloured value together
// (zStoreBarrierBuffer.hpp:33-39).  The installation state is needed when an entry
// crosses a colour publication: ownership follows the install-time generation and
// mark colour recorded on the entry (zStoreBarrierBuffer.cpp:190-218).
struct StoreBarrierInstallState {
    uint8_t phase = 0;
    bool youngMark = false;
    uintptr_t storeGood = 0;
};

struct StoreBarrierEntry {
    MAddress p = 0;
    zpointer prev = zpointer::null;
    StoreBarrierInstallState installed {};
};

#if defined(MRT_GC_UNIT_TESTS)
enum class StoreBarrierFlushEvent : uint8_t {
    PREVIOUS_RETIRED,
    SLOT_REMEMBERED,
};
using StoreBarrierFlushObserver = void (*)(StoreBarrierFlushEvent, const StoreBarrierEntry&);
#endif

class StoreBarrierBuffer {
public:
    StoreBarrierBuffer() : current(BufferLength) {}

    static constexpr size_t Capacity() { return BufferLength; }
    bool IsEmpty() const { return current == BufferLength; }
    size_t Pending() const { return BufferLength - current; }
    size_t Current() const { return current; }

    void Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs);
    void Flush(RememberedSet& rs);

    static void FlushAll(RememberedSet& rs);
#if defined(MRT_GC_UNIT_TESTS)
    static void SetFlushObserverForTest(StoreBarrierFlushObserver observer);
#endif

private:
    static constexpr size_t BufferLength = 32;

    static StoreBarrierInstallState CaptureInstallState();
    static bool InstalledDuringCurrentMark(const StoreBarrierEntry& entry);
    static bool RetirePrevious(const StoreBarrierEntry& entry, Collector& collector);
    static void MarkAndRemember(const StoreBarrierEntry& entry, RememberedSet& rs);
    void Add(MAddress fieldAddress, zpointer prev, StoreBarrierInstallState installed, RememberedSet& rs);
    void Flush(RememberedSet& rs, Collector& collector);

    StoreBarrierEntry buffer[BufferLength] {};
    size_t current;
};
} // namespace MapleRuntime

#endif
