// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FixEdgeSet.h"

#include <algorithm>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
FixEdgeSet& FixEdgeSet::Instance() noexcept
{
    static FixEdgeSet instance;
    return instance;
}

void FixEdgeSet::Add(MAddress slotAddr, uint64_t slotEpoch, bool hasSlotEpoch)
{
    if (slotAddr == 0) {
        return;
    }
    Entry e;
    e.slotAddr = slotAddr;
    e.slotEpoch = slotEpoch;
    e.hasSlotEpoch = hasSlotEpoch;
    std::lock_guard<std::mutex> lg(mutex);
    slots.push_back(e);
    count.fetch_add(1, std::memory_order_relaxed);
}

void FixEdgeSet::MaybeAdd(BaseObject* holder, RefField<>* slot, BaseObject* newRef)
{
    if (slot == nullptr || newRef == nullptr) {
        return;
    }
    if (!Heap::IsHeapAddress(newRef)) {
        return;
    }
    // Slot must remain at the same absolute address until BulkForward. If the
    // holder is itself in from/ghost, evacuation moves the field — skip (roots
    // and to-space holders only). Static roots: holder == nullptr.
    RegionInfo* slotRegion = nullptr;
    if (holder != nullptr) {
        if (!Heap::IsHeapAddress(holder)) {
            return;
        }
        if (RegionInfo::InGhostFromRegion(holder)) {
            return;
        }
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
        if (hr != nullptr && hr->IsFromRegion()) {
            return;
        }
        slotRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(slot));
    } else {
        slotRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(slot));
    }

    auto stampAndAdd = [&]() {
        // R2: stamp slot-region epoch only (E9 constructive). ⛔ no target stamp.
        const uint64_t sEpoch = slotRegion != nullptr ? slotRegion->GetEpoch() : 0;
        const bool hasS = slotRegion != nullptr;
        Add(reinterpret_cast<MAddress>(slot), sEpoch, hasS);
    };

    // I5: only edges whose target is already From or GhostFrom need bulk fix.
    // plainsrc P11 (Idle plain→then-from) is covered by Trace I4 complement when
    // the holder is scanned; unreached holders need compiler dual-track (r1cc).
    if (RegionInfo::InGhostFromRegion(newRef)) {
        stampAndAdd();
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(newRef));
    if (region != nullptr && region->IsFromRegion()) {
        stampAndAdd();
    }
}

void FixEdgeSet::VisitAndClear(const SlotVisitor& visitor)
{
    std::vector<Entry> local;
    {
        std::lock_guard<std::mutex> lg(mutex);
        local.swap(slots);
        count.store(0, std::memory_order_relaxed);
    }
    if (local.empty()) {
        return;
    }
    std::sort(local.begin(), local.end(),
              [](const Entry& a, const Entry& b) { return a.slotAddr < b.slotAddr; });
    local.erase(std::unique(local.begin(), local.end(),
                            [](const Entry& a, const Entry& b) { return a.slotAddr == b.slotAddr; }),
                local.end());
    for (const Entry& e : local) {
        const MAddress addr = e.slotAddr;
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            continue;
        }
        RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(static_cast<uintptr_t>(addr));
        if (slotRegion == nullptr) {
            continue;
        }
        // Slot-epoch first (definitional expiry). Parallel E9 free/garbage gates retained
        // for observation: epoch_skip vs e9_gate should cover the same set when complete.
        if (e.hasSlotEpoch && slotRegion->GetEpoch() != e.slotEpoch) {
            epochSkipCount.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<size_t> sample{ 0 };
            size_t n = sample.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixEdgeSet] epoch_skip slot region=%p epoch_seen=%llu epoch_now=%llu n=%zu",
                     slotRegion, static_cast<unsigned long long>(e.slotEpoch),
                     static_cast<unsigned long long>(slotRegion->GetEpoch()), n);
            }
            continue;
        }
        // E9 hard gate (parallel observe): free/garbage/from/ghost slot regions.
        if (slotRegion->IsGhostFromRegion() || slotRegion->IsFromRegion() || slotRegion->IsFreeRegion() ||
            slotRegion->IsGarbageRegion()) {
            e9GateSkipCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // P-G: touch only indexed field addresses — no object walk / GetSize.
        auto* field = reinterpret_cast<RefField<>*>(addr);
        visitor(*field);
    }
}
} // namespace MapleRuntime
