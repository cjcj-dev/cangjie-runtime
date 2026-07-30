// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_SIGB_WRITER_PROVENANCE_H
#define MRT_SIGB_WRITER_PROVENANCE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class TypeInfo;

// gcsigb2 observe-only: exact-slot writer provenance for signature-B victim
// instances (ConstAnalysisWrapper.resultsMap backing RawArrays). Never selects
// or rewrites a reference; range checks are two comparisons on hot paths.
// hidden: observe-only, must not add dynamic exports (nm -D gate).
class __attribute__((visibility("hidden"))) SigBWriterProvenance {
public:
    enum WriterKind : uint8_t {
        WRITER_NONE = 0,
        WRITER_BULK_FIX = 1,
        WRITER_BARRIER = 2,
        WRITER_COPY_OBJECT = 3,
    };

    struct RegistryEntry {
        uintptr_t start{ 0 };
        uintptr_t end{ 0 };
        TypeInfo* typeInfo{ nullptr };
    };

    struct RingEntry {
        uint8_t writer{ WRITER_NONE };
        uint8_t phase{ 0 };
        uint16_t pad{ 0 };
        uint32_t gcOrdinal{ 0 };
        uintptr_t slot{ 0 };
        uintptr_t oldValue{ 0 };
        uintptr_t newValue{ 0 };
        uintptr_t holder{ 0 };
    };

    static constexpr size_t REGISTRY_CAP = 64;
    static constexpr size_t RING_CAP = 1024; // 2^10
    static constexpr size_t RING_MASK = RING_CAP - 1;

    struct ThreadRing {
        RingEntry entries[RING_CAP];
        std::atomic<uint32_t> cursor{ 0 };
        std::atomic<uint32_t> total{ 0 };
        ThreadRing* next{ nullptr };
        uint64_t tid{ 0 };
    };

    static SigBWriterProvenance& Instance() noexcept;

    // Alloc path: register resultsMap-domain RawArray [start,end).
    void MaybeRegister(BaseObject* obj, TypeInfo* typeInfo, size_t size) noexcept;

    // CopyObject: relocate registry rows whose from-range matches; log if the
    // destination range overlaps any registered victim range.
    void OnCopyObject(BaseObject* from, BaseObject* to, size_t size, TypeInfo* typeInfo) noexcept;

    // Bulk fix / barrier write: log when the destination slot is inside a
    // registered victim range.
    void MaybeLogWrite(WriterKind kind, void* slot, BaseObject* holder, uintptr_t oldValue,
                       uintptr_t newValue) noexcept;

    // Crash / abort / atexit dump: registry + thread rings to
    // ${MRT_FATAL_ATTR_PATH}.sigbwriter.<pid>.
    void Dump() noexcept;
    void EnsureExitDumpRegistered() noexcept;
    void LogSummary() noexcept;

    size_t RegistrySize() const noexcept { return registrySize.load(std::memory_order_relaxed); }
    size_t OverflowCount() const noexcept { return overflowCount.load(std::memory_order_relaxed); }
    size_t WriteHitCount() const noexcept { return writeHitCount.load(std::memory_order_relaxed); }

private:
    SigBWriterProvenance() = default;
    ~SigBWriterProvenance() = default;
    SigBWriterProvenance(const SigBWriterProvenance&) = delete;
    SigBWriterProvenance& operator=(const SigBWriterProvenance&) = delete;

    static bool IsConstAnalysisVictimType(TypeInfo* typeInfo) noexcept;
    bool ContainsSlot(uintptr_t slot) const noexcept;
    void RecordRing(WriterKind kind, uintptr_t slot, BaseObject* holder, uintptr_t oldValue,
                    uintptr_t newValue) noexcept;

    ThreadRing* GetThreadRing() noexcept;
    void DumpRing(int fd, const ThreadRing* ring) const noexcept;
    void DumpRegistry(int fd) const noexcept;
    static void WriteText(int fd, const char* text) noexcept;
    static void WriteHex(int fd, uintptr_t value) noexcept;
    static void WriteDec(int fd, uint64_t value) noexcept;

    mutable std::mutex registryMutex;
    RegistryEntry registry[REGISTRY_CAP];
    std::atomic<size_t> registrySize{ 0 };
    std::atomic<size_t> overflowCount{ 0 };
    std::atomic<size_t> writeHitCount{ 0 };
    std::atomic<size_t> copyHitCount{ 0 };
    std::atomic<size_t> relocateCount{ 0 };
    std::atomic<size_t> hashMapEntryAllocCount{ 0 };

    std::mutex ringListMutex;
    ThreadRing* ringListHead{ nullptr };
    std::atomic<bool> dumped{ false };
    std::atomic<bool> exitHookRegistered{ false };
};
} // namespace MapleRuntime

#endif // MRT_SIGB_WRITER_PROVENANCE_H
