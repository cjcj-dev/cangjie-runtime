// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARD_FACT_TABLE_H
#define MRT_FORWARD_FACT_TABLE_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_map>

#include "Common/TypeDef.h"

namespace MapleRuntime {
// R2.1 copy-time fact carrier (EPOCH_DESIGN_0729 R2.1 / r1route2).
// Written only when CopyObject has both from and to addresses in hand.
// Consumed only by FixHolder under BulkForward STW. Cleared at BulkForward end
// (same major as FixEdgeSet VisitAndClear). No epoch stamp — lifetime is the
// self-invalidating window [copy write, BulkForward clear].
//
// Named ForwardFactTable (not ForwardTable) to avoid clash with the existing
// WCollector::ForwardTable route facade (RouteObject/PrepareForwardTable).
//
// Concurrent writers: GC thread-pool Forward/Preforward/Compact paths. Object
// lock ensures one copy per from-object; distinct keys may insert concurrently.
class ForwardFactTable {
public:
    static ForwardFactTable& Instance() noexcept;

    // Copy-time only. Never write a half entry: call only after CopyObject body
    // has both addresses (and before from header is zeroed/unlocked as FORWARDED).
    void Record(BaseObject* from, BaseObject* to);

    // STW BulkForward consumer. Returns nullptr if no copy-time fact for from.
    BaseObject* Lookup(BaseObject* from) const;

    // End of BulkForward (with FixEdgeSet VisitAndClear). Drops all entries.
    void Clear();

    size_t SizeApprox() const { return count.load(std::memory_order_relaxed); }

private:
    ForwardFactTable() = default;
    ~ForwardFactTable() = default;
    ForwardFactTable(const ForwardFactTable&) = delete;
    ForwardFactTable& operator=(const ForwardFactTable&) = delete;

    mutable std::mutex mutex;
    std::unordered_map<BaseObject*, BaseObject*> table;
    std::atomic<size_t> count{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_FORWARD_FACT_TABLE_H
