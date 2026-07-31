// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FIX_EDGE_SET_H
#define MRT_FIX_EDGE_SET_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class BaseObject;

// R1 edge fix-set (REPORT-holderidx I5 + P-G): concurrent index of ref-field
// offsets grouped by their holder identity. Heap slots are never retained as
// bare addresses: copy/reclaim paths relocate or invalidate the holder carrier.
//
// Lifetime: entry established on runtime heap-ref store (or Trace plain→from),
// relocated or invalidated with its holder, then cleared at the end of the same
// major's BulkForward. Add/copy/reclaim are mutex-serialized; VisitAndClear is STW.
class FixEdgeSet {
public:
    static FixEdgeSet& Instance() noexcept;

    // Register a process-lifetime slot without a heap holder (static roots).
    void Add(MAddress slotAddr);

    // Register when newRef is already From/GhostFrom (I5), or — when GC phase is
    // active (phase > INIT, slow-path barriers only) — any cross-region heap ref
    // store (E6 (i)-narrow). Idle does not widen. holder may be null (static root).
    // Skips stores into holders that are already from/ghost; carriers registered
    // earlier are relocated by CopyObject when their holder evacuates.
    void MaybeAdd(BaseObject* holder, RefField<>* slot, BaseObject* newRef);

    // Collector copy/reclaim hooks. RelocateHolder rebases all indexed field offsets
    // and removes carriers overwritten by the destination range. InvalidateRange
    // closes the carrier lifetime before storage is reclaimed or reused.
    void RelocateHolder(MAddress from, MAddress to, size_t size);
    void InvalidateRange(MAddress start, size_t size);

    // STW-only: visit each registered slot once (unique within each holder).
    using SlotVisitor = std::function<void(BaseObject* holder, RefField<>& field)>;
    void VisitAndClear(const SlotVisitor& visitor);

    size_t SizeApprox() const { return count.load(std::memory_order_relaxed); }
    size_t E9GateSkipCount() const { return e9GateSkipCount.load(std::memory_order_relaxed); }
    void ResetE9GateSkipCount() { e9GateSkipCount.store(0, std::memory_order_relaxed); }

    // Observe-only census for registered slots whose holders moved. These
    // counters never select a rewrite or change a slot.
    size_t CopyDstFactCount() const { return copyDstFactCount.load(std::memory_order_relaxed); }
    size_t CopyDstNoFactCount() const { return copyDstNoFactCount.load(std::memory_order_relaxed); }
    size_t CopyDstStaleTargetCount() const { return copyDstStaleTargetCount.load(std::memory_order_relaxed); }
    size_t CopyDstConstDomainFactCount() const
    {
        return copyDstConstDomainFactCount.load(std::memory_order_relaxed);
    }
    size_t CopyDstConstDomainStaleTargetCount() const
    {
        return copyDstConstDomainStaleTargetCount.load(std::memory_order_relaxed);
    }
    size_t CopyDstConstPoolDomainFactCount() const
    {
        return copyDstConstPoolDomainFactCount.load(std::memory_order_relaxed);
    }
    size_t CopyDstConstPoolDomainStaleTargetCount() const
    {
        return copyDstConstPoolDomainStaleTargetCount.load(std::memory_order_relaxed);
    }

    // E6 observability (relaxed; never used as a silent-skip gate).
    size_t CrossRegionRegistered() const { return crossRegionRegistered.load(std::memory_order_relaxed); }
    size_t FromTargetRegistered() const { return fromTargetRegistered.load(std::memory_order_relaxed); }

private:
    struct HolderEntry {
        size_t holderSize;
        uint64_t regionIdentityEpoch;
        std::vector<size_t> fieldOffsets;
        MAddress relocationSource{ 0 };
    };

    FixEdgeSet() = default;
    ~FixEdgeSet() = default;
    FixEdgeSet(const FixEdgeSet&) = delete;
    FixEdgeSet& operator=(const FixEdgeSet&) = delete;

    void AddHolder(MAddress holder, size_t holderSize, size_t fieldOffset, uint64_t regionIdentityEpoch);
    size_t InvalidateRangeLocked(MAddress start, size_t size);

    std::mutex mutex;
    std::map<MAddress, HolderEntry> holders;
    std::vector<MAddress> staticSlots;
    std::atomic<size_t> count{ 0 };
    std::atomic<size_t> heapSlotCount{ 0 };
    std::atomic<size_t> e9GateSkipCount{ 0 };
    // Carrier rebasing makes NO_FACT structurally zero. Keep the field only for
    // log-schema continuity; escaped-carrier fail-stop is the missing-hook guard.
    std::atomic<size_t> copyDstFactCount{ 0 };
    std::atomic<size_t> copyDstNoFactCount{ 0 };
    std::atomic<size_t> copyDstStaleTargetCount{ 0 };
    std::atomic<size_t> copyDstConstDomainFactCount{ 0 };
    std::atomic<size_t> copyDstConstDomainStaleTargetCount{ 0 };
    std::atomic<size_t> copyDstConstPoolDomainFactCount{ 0 };
    std::atomic<size_t> copyDstConstPoolDomainStaleTargetCount{ 0 };
    std::atomic<size_t> crossRegionRegistered{ 0 };
    std::atomic<size_t> fromTargetRegistered{ 0 };
    std::atomic<size_t> relocatedSlots{ 0 };
    std::atomic<size_t> invalidatedSlots{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_FIX_EDGE_SET_H
