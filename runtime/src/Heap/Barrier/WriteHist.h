// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
// Observation-only write-history ring for remset miss attribution (gcmissedge).
// Last-write map keyed by field address; capacity sized to avoid wrap false-negatives.

#ifndef MRT_WRITE_HIST_H
#define MRT_WRITE_HIST_H

#include <atomic>
#include <cstdint>
#include <cstring>

#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

namespace MapleRuntime {

enum class CrossGenWriteApi : uint8_t {
    NONE = 0,
    WRITE_REF = 1,
    WRITE_STRUCT = 2,
    ATOMIC_WRITE = 3,
    ATOMIC_SWAP = 4,
    CAS = 5,
    COPY_REF_ARR = 6,
    COPY_STRUCT_ARR = 7,
    WRITE_GENERIC = 8,
    READ_GENERIC = 9,
};

enum class CrossGenRecordOutcome : uint8_t {
    NONE = 0,
    RECORDED = 1,
    EARLY_NO_YOUNG_REGIONS = 2,
    EARLY_NULL_OR_NON_HEAP = 3,
    EARLY_TARGET_NOT_YOUNG = 4,
    EARLY_SOURCE_IS_YOUNG = 5,
};

struct WriteHistEntry {
    MAddress fieldAddress { 0 };
    BaseObject* holder { nullptr };
    BaseObject* ref { nullptr };
    uint32_t seq { 0 };
    uint32_t epoch { 0 };
    uint8_t api { 0 };
    uint8_t phase { 0 };
    uint8_t outcome { 0 };
    uint8_t pad { 0 };
};

// Power-of-two open-addressing last-write table. 1M slots ≈ 40MB.
// Collision with a different field overwrites (counted as wrap).
class WriteHist {
public:
    static constexpr size_t kCapacity = 1u << 20; // 1048576
    static constexpr size_t kMask = kCapacity - 1;

    static WriteHist& Instance()
    {
        static WriteHist hist;
        return hist;
    }

    void BumpEpoch() { epoch_.fetch_add(1, std::memory_order_relaxed); }
    uint32_t Epoch() const { return epoch_.load(std::memory_order_relaxed); }

    void Note(MAddress fieldAddress, BaseObject* holder, BaseObject* ref, CrossGenWriteApi api, GCPhase phase,
              CrossGenRecordOutcome outcome)
    {
        if (fieldAddress == 0) {
            return;
        }
        const uint32_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
        size_t idx = Hash(fieldAddress);
        Slot& slot = slots_[idx];
        MAddress prev = slot.field.load(std::memory_order_relaxed);
        if (prev != 0 && prev != fieldAddress) {
            wraps_.fetch_add(1, std::memory_order_relaxed);
        }
        // Publish field last so readers under STW see a consistent-enough snapshot.
        slot.entry.holder = holder;
        slot.entry.ref = ref;
        slot.entry.seq = seq;
        slot.entry.epoch = epoch;
        slot.entry.api = static_cast<uint8_t>(api);
        slot.entry.phase = static_cast<uint8_t>(phase);
        slot.entry.outcome = static_cast<uint8_t>(outcome);
        slot.entry.fieldAddress = fieldAddress;
        slot.field.store(fieldAddress, std::memory_order_release);
        total_.fetch_add(1, std::memory_order_relaxed);
        outcomeCount_[static_cast<uint8_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
        apiCount_[static_cast<uint8_t>(api)].fetch_add(1, std::memory_order_relaxed);
    }

    bool Lookup(MAddress fieldAddress, WriteHistEntry& out) const
    {
        if (fieldAddress == 0) {
            return false;
        }
        size_t idx = Hash(fieldAddress);
        const Slot& slot = slots_[idx];
        if (slot.field.load(std::memory_order_acquire) != fieldAddress) {
            return false;
        }
        out = slot.entry;
        return out.fieldAddress == fieldAddress;
    }

    uint64_t Total() const { return total_.load(std::memory_order_relaxed); }
    uint64_t Wraps() const { return wraps_.load(std::memory_order_relaxed); }
    uint64_t OutcomeCount(CrossGenRecordOutcome o) const
    {
        return outcomeCount_[static_cast<uint8_t>(o)].load(std::memory_order_relaxed);
    }
    uint64_t ApiCount(CrossGenWriteApi a) const
    {
        return apiCount_[static_cast<uint8_t>(a)].load(std::memory_order_relaxed);
    }

    static const char* ApiName(uint8_t api)
    {
        switch (static_cast<CrossGenWriteApi>(api)) {
            case CrossGenWriteApi::WRITE_REF:
                return "WriteReference";
            case CrossGenWriteApi::WRITE_STRUCT:
                return "WriteStruct";
            case CrossGenWriteApi::ATOMIC_WRITE:
                return "AtomicWriteReference";
            case CrossGenWriteApi::ATOMIC_SWAP:
                return "AtomicSwapReference";
            case CrossGenWriteApi::CAS:
                return "CompareAndSwapReference";
            case CrossGenWriteApi::COPY_REF_ARR:
                return "CopyRefArray";
            case CrossGenWriteApi::COPY_STRUCT_ARR:
                return "CopyStructArray";
            case CrossGenWriteApi::WRITE_GENERIC:
                return "WriteGeneric";
            case CrossGenWriteApi::READ_GENERIC:
                return "ReadGeneric";
            default:
                return "none";
        }
    }

    static const char* OutcomeName(uint8_t outcome)
    {
        switch (static_cast<CrossGenRecordOutcome>(outcome)) {
            case CrossGenRecordOutcome::RECORDED:
                return "RECORDED";
            case CrossGenRecordOutcome::EARLY_NO_YOUNG_REGIONS:
                return "EARLY_NO_YOUNG_REGIONS";
            case CrossGenRecordOutcome::EARLY_NULL_OR_NON_HEAP:
                return "EARLY_NULL_OR_NON_HEAP";
            case CrossGenRecordOutcome::EARLY_TARGET_NOT_YOUNG:
                return "EARLY_TARGET_NOT_YOUNG";
            case CrossGenRecordOutcome::EARLY_SOURCE_IS_YOUNG:
                return "EARLY_SOURCE_IS_YOUNG";
            default:
                return "NONE";
        }
    }

private:
    struct Slot {
        std::atomic<MAddress> field { 0 };
        WriteHistEntry entry {};
    };

    WriteHist() = default;

    static size_t Hash(MAddress fieldAddress)
    {
        // field addresses are pointer-aligned; mix high bits.
        uintptr_t x = static_cast<uintptr_t>(fieldAddress);
        x ^= x >> 12;
        x *= 0x9e3779b97f4a7c15ull;
        x ^= x >> 32;
        return static_cast<size_t>(x) & kMask;
    }

    Slot slots_[kCapacity];
    std::atomic<uint32_t> seq_ { 0 };
    std::atomic<uint32_t> epoch_ { 0 };
    std::atomic<uint64_t> total_ { 0 };
    std::atomic<uint64_t> wraps_ { 0 };
    std::atomic<uint64_t> outcomeCount_[16] {};
    std::atomic<uint64_t> apiCount_[16] {};
};

} // namespace MapleRuntime

#endif // MRT_WRITE_HIST_H
