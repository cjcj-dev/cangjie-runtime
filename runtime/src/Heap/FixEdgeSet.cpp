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

void FixEdgeSet::Add(MAddress slotAddr)
{
    if (slotAddr == 0) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    slots.push_back(slotAddr);
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
    RegionInfo* holderRegion = nullptr;
    if (holder != nullptr) {
        if (!Heap::IsHeapAddress(holder)) {
            return;
        }
        if (RegionInfo::InGhostFromRegion(holder)) {
            return;
        }
        holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
        if (holderRegion != nullptr && holderRegion->IsFromRegion()) {
            return;
        }
    }
    // I5: target already From or GhostFrom → always register.
    if (RegionInfo::InGhostFromRegion(newRef)) {
        fromTargetRegistered.fetch_add(1, std::memory_order_relaxed);
        Add(reinterpret_cast<MAddress>(slot));
        return;
    }
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(newRef));
    if (targetRegion != nullptr && targetRegion->IsFromRegion()) {
        fromTargetRegistered.fetch_add(1, std::memory_order_relaxed);
        Add(reinterpret_cast<MAddress>(slot));
        return;
    }
    // E6 (i)-narrow: only while GC phase is active (phase > INIT ⇒ mutator already
    // on slow-path barriers). Idle keeps I5-only (no FixSet inflation). Register
    // every cross-region heap ref store so live holders that later miss I4 still
    // enter the set (closes T1 black-allocation / concurrent-phase escape).
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    if (phase <= GCPhase::GC_PHASE_INIT) {
        return;
    }
    if (holder == nullptr || holderRegion == nullptr || targetRegion == nullptr) {
        return;
    }
    if (holderRegion == targetRegion) {
        return;
    }
    crossRegionRegistered.fetch_add(1, std::memory_order_relaxed);
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
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            continue;
        }
        // Slot must sit in a non-ghost region (holder not evacuated).
        RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(static_cast<uintptr_t>(addr));
        if (slotRegion == nullptr || slotRegion->IsGhostFromRegion() || slotRegion->IsFromRegion() ||
            slotRegion->IsFreeRegion() || slotRegion->IsGarbageRegion()) {
            continue;
        }
        // P-G: touch only indexed field addresses — no object walk / GetSize.
        auto* field = reinterpret_cast<RefField<>*>(addr);
        visitor(*field);
    }
}
} // namespace MapleRuntime
