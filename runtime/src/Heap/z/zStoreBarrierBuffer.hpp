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
constexpr bool kBufferStoreBarriers = true;
constexpr size_t kStoreBarrierBufferLength = 32;

struct StoreBarrierEntry {
    volatile zpointer* p = nullptr;
    zpointer prev = zpointer::null;
    static constexpr size_t p_offset() { return offsetof(StoreBarrierEntry, p); }
    static constexpr size_t prev_offset() { return offsetof(StoreBarrierEntry, prev); }
};

class StoreBarrierBuffer {
public:
    StoreBarrierBuffer();
    void Initialize(uintptr_t color);

    bool IsEmpty() const;
    size_t Current() const;
    size_t Pending() const { return kStoreBarrierBufferLength - Current(); }
    static constexpr size_t Capacity() { return kStoreBarrierBufferLength; }
    static StoreBarrierBuffer* buffer_for_store(bool heal);

    void add(volatile zpointer* p, zpointer prev);
    void Flush();
    void install_base_pointers();
    void on_new_phase();
    static bool is_in(MAddress p);

    friend class MutatorManager;
    StoreBarrierEntry buffer[kStoreBarrierBufferLength] {};

private:
    void clear();
    void install_base_pointers_inner();
    void on_new_phase_relocate(size_t i);
    void on_new_phase_remember(size_t i);
    void on_new_phase_mark(size_t i);
    bool is_old_mark() const;
    bool stored_during_old_mark() const;

public:
    uintptr_t lastProcessedColor;
    uintptr_t lastInstalledColor;
    std::mutex basePointerLock;
    zaddress_unsafe basePointers[kStoreBarrierBufferLength] {};
    // ZGC zStoreBarrierBuffer.hpp:61: byte index growing downwards.
    size_t current;

public:
    static constexpr size_t BufferSizeBytes = kStoreBarrierBufferLength * sizeof(StoreBarrierEntry);
    static constexpr size_t buffer_offset() { return offsetof(StoreBarrierBuffer, buffer); }
    static constexpr size_t current_offset() { return offsetof(StoreBarrierBuffer, current); }
};

// Paired with StoreBarrierBufferABI in LLVM CJBarrierLowering.cpp. current
// follows std::mutex, whose representation is platform-dependent. Only the
// x86_64 Linux GNU LP64 target uses this fixed table; other targets continue
// to load the five exported layout values from CompilerCalls.cpp.
#if defined(__linux__) && defined(__GLIBC__) && defined(__x86_64__) && !defined(__ILP32__)
namespace StoreBarrierBufferABI {
constexpr size_t Current = 824;
constexpr size_t Buffer = 0;
constexpr size_t EntrySize = 16;
constexpr size_t EntryP = 0;
constexpr size_t EntryPrev = 8;
} // namespace StoreBarrierBufferABI
static_assert(sizeof(void*) == 8 && sizeof(size_t) == 8 && sizeof(uintptr_t) == 8 &&
              sizeof(zpointer) == 8, "compiler store buffer LP64 ABI");
static_assert(StoreBarrierBuffer::current_offset() == StoreBarrierBufferABI::Current,
              "compiler store buffer current ABI");
static_assert(StoreBarrierBuffer::buffer_offset() == StoreBarrierBufferABI::Buffer,
              "compiler store buffer entries ABI");
static_assert(sizeof(StoreBarrierEntry) == StoreBarrierBufferABI::EntrySize,
              "compiler store buffer entry size ABI");
static_assert(StoreBarrierEntry::p_offset() == StoreBarrierBufferABI::EntryP,
              "compiler store buffer entry p ABI");
static_assert(StoreBarrierEntry::prev_offset() == StoreBarrierBufferABI::EntryPrev,
              "compiler store buffer entry prev ABI");
#endif

} // namespace MapleRuntime

#include "Heap/z/zStoreBarrierBuffer.inline.hpp"
#endif
