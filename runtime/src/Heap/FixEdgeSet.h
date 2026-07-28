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
// No cross-major retention. Mutator Add vs STW Visit/Clear: STW excludes mutators.
class FixEdgeSet {
public:
    static FixEdgeSet& Instance() noexcept;

    // Register a heap ref slot that was just written (or Trace-observed).
    // slotAddr = absolute address of RefField<> storage (edge key).
    void Add(MAddress slotAddr);

    // Convenience: register when newRef is a heap object pointer.
    void MaybeAdd(RefField<>* slot, BaseObject* newRef);

    // STW-only: visit each registered slot once (best-effort unique via sort+unique).
    using SlotVisitor = std::function<void(RefField<>& field)>;
    void VisitAndClear(const SlotVisitor& visitor);

    size_t SizeApprox() const { return count.load(std::memory_order_relaxed); }

private:
    FixEdgeSet() = default;
    ~FixEdgeSet() = default;
    FixEdgeSet(const FixEdgeSet&) = delete;
    FixEdgeSet& operator=(const FixEdgeSet&) = delete;

    void FlushLocked();

    static constexpr size_t LOCAL_CAP = 256;

    std::mutex mutex;
    std::vector<MAddress> slots;
    std::atomic<size_t> count{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_FIX_EDGE_SET_H
