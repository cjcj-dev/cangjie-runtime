// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FixEdgeSet.h"

#include <algorithm>

#include "Base/Log.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
FixEdgeSet& FixEdgeSet::Instance() noexcept
{
    static FixEdgeSet instance;
    return instance;
}

void FixEdgeSet::Add(MAddress slotAddr)
{
    if (slotAddr == 0) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    slots.push_back(slotAddr);
    count.fetch_add(1, std::memory_order_relaxed);
}

void FixEdgeSet::MaybeAdd(RefField<>* slot, BaseObject* newRef)
{
    if (slot == nullptr || newRef == nullptr) {
        return;
    }
    // Runtime-track completeness for plain→later-from (plainsrc P1/P11): register
    // every heap-object ref store that passes a runtime barrier. Consume-side
    // predicates filter to ghost-from survivors with route. Compiler Idle plain
    // stores (P5) do not enter here — residual expected until r1cc.
    if (!Heap::IsHeapAddress(newRef)) {
        return;
    }
    Add(reinterpret_cast<MAddress>(slot));
}

void FixEdgeSet::VisitAndClear(const SlotVisitor& visitor)
{
    std::vector<MAddress> local;
    {
        std::lock_guard<std::mutex> lg(mutex);
        local.swap(slots);
        count.store(0, std::memory_order_relaxed);
    }
    if (local.empty()) {
        return;
    }
    std::sort(local.begin(), local.end());
    local.erase(std::unique(local.begin(), local.end()), local.end());
    for (MAddress addr : local) {
        if (addr == 0) {
            continue;
        }
        // P-G: touch only indexed field addresses — no object walk / GetSize.
        auto* field = reinterpret_cast<RefField<>*>(addr);
        visitor(*field);
    }
}
} // namespace MapleRuntime
