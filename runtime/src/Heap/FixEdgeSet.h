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
#include <mutex>
#include <vector>

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class BaseObject;

// R1 edge fix-set (REPORT-holderidx I5 + P-G): concurrent bag of ref-field slot
// absolute addresses produced at store (and optional Trace) time; consumed only
// by BulkForwardHolderRefs under STW. Key = edge (field slot), not line/region/obj.
//
// Lifetime: entry established on runtime heap-ref store (or Trace plain→from);
// invalidated by Clear() at end of BulkForward (same major, after ForwardFromSpace).
// Epoch stamps (EPOCH_DESIGN_0729 R2): slot-region epoch at registration only;
// ⛔ no target-region stamp/compare (LIE-1). Consumer rejects on slot-epoch mismatch
// (definitional expiry; E9 free/garbage kept as parallel observe — same intercept set).
// No cross-major retention. Mutator Add vs STW Visit/Clear: STW excludes mutators.
// Public Add(MAddress) signature preserved (export surface / mutator ABI unchanged).
class FixEdgeSet {
public:
    struct Entry {
        MAddress slotAddr = 0;
        uint64_t slotEpoch = 0;
        bool hasSlotEpoch = false;
    };

    static FixEdgeSet& Instance() noexcept;

    // Register a heap ref slot that was just written (or Trace-observed).
    // slotAddr = absolute address of RefField<> storage (edge key).
    // Stamps slot-region epoch internally when the address is a heap region.
    void Add(MAddress slotAddr);

    // Test/harness: register with explicit slot epoch (not on export path).
    void AddWithEpoch(MAddress slotAddr, uint64_t slotEpoch, bool hasSlotEpoch);

    // Register when newRef is already From/GhostFrom (I5). holder may be null
    // (static root). Skips stores into from/ghost holders (slot would evacuate).
    void MaybeAdd(BaseObject* holder, RefField<>* slot, BaseObject* newRef);

    // STW-only: visit each registered slot once (best-effort unique via sort+unique).
    // Slot-epoch mismatch and E9 free/garbage skips are counted before visitor.
    using SlotVisitor = std::function<void(RefField<>& field)>;
    void VisitAndClear(const SlotVisitor& visitor);

    size_t SizeApprox() const { return count.load(std::memory_order_relaxed); }

    // Observability (relaxed; loud skips, never silent).
    size_t EpochSkipCount() const { return epochSkipCount.load(std::memory_order_relaxed); }
    size_t E9GateSkipCount() const { return e9GateSkipCount.load(std::memory_order_relaxed); }
    void ResetSkipCounts()
    {
        epochSkipCount.store(0, std::memory_order_relaxed);
        e9GateSkipCount.store(0, std::memory_order_relaxed);
    }

private:
    FixEdgeSet() = default;
    ~FixEdgeSet() = default;
    FixEdgeSet(const FixEdgeSet&) = delete;
    FixEdgeSet& operator=(const FixEdgeSet&) = delete;

    static constexpr size_t LOCAL_CAP = 256;

    std::mutex mutex;
    std::vector<Entry> slots;
    std::atomic<size_t> count{ 0 };
    std::atomic<size_t> epochSkipCount{ 0 };
    std::atomic<size_t> e9GateSkipCount{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_FIX_EDGE_SET_H
