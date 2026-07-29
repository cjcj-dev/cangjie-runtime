// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FixEdgeSet.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {
// epochPacked: 0 = unstamped; else region.epoch + 1.
inline MAddress PackEpoch(uint64_t epoch, bool has)
{
    if (!has) {
        return 0;
    }
    return static_cast<MAddress>(epoch + 1);
}
inline bool UnpackHasEpoch(MAddress packed) { return packed != 0; }
inline uint64_t UnpackEpoch(MAddress packed) { return static_cast<uint64_t>(packed) - 1; }
} // namespace

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
    // R2: stamp slot-region epoch only (E9 constructive). ⛔ no target stamp.
    MAddress epochPacked = 0;
    if (Heap::IsHeapAddress(slotAddr)) {
        RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(static_cast<uintptr_t>(slotAddr));
        if (slotRegion != nullptr) {
            epochPacked = PackEpoch(slotRegion->GetEpoch(), true);
        }
    }
    std::lock_guard<std::mutex> lg(mutex);
    slots.push_back(slotAddr);
    slots.push_back(epochPacked);
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
    }

    // I5: only edges whose target is already From or GhostFrom need bulk fix.
    // plainsrc P11 (Idle plain→then-from) is covered by Trace I4 complement when
    // the holder is scanned; unreached holders need compiler dual-track (r1cc).
    if (RegionInfo::InGhostFromRegion(newRef)) {
        Add(reinterpret_cast<MAddress>(slot));
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(newRef));
    if (region != nullptr && region->IsFromRegion()) {
        Add(reinterpret_cast<MAddress>(slot));
    }
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
    // Collapse to (slot, epochPacked) pairs, unique by slot (keep first stamp).
    std::vector<std::pair<MAddress, MAddress>> pairs;
    pairs.reserve(local.size() / 2);
    for (size_t i = 0; i + 1 < local.size(); i += 2) {
        pairs.emplace_back(local[i], local[i + 1]);
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const std::pair<MAddress, MAddress>& a, const std::pair<MAddress, MAddress>& b) {
                  return a.first < b.first;
              });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
                            [](const std::pair<MAddress, MAddress>& a, const std::pair<MAddress, MAddress>& b) {
                                return a.first == b.first;
                            }),
                pairs.end());
    for (const auto& p : pairs) {
        const MAddress addr = p.first;
        const MAddress epochPacked = p.second;
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            continue;
        }
        RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(static_cast<uintptr_t>(addr));
        if (slotRegion == nullptr) {
            continue;
        }
        // Slot-epoch first (definitional expiry). Parallel E9 free/garbage gates retained
        // for observation: epoch_skip vs e9_gate should cover the same set when complete.
        if (UnpackHasEpoch(epochPacked) && slotRegion->GetEpoch() != UnpackEpoch(epochPacked)) {
            epochSkipCount.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<size_t> sample{ 0 };
            size_t n = sample.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixEdgeSet] epoch_skip slot region=%p epoch_seen=%llu epoch_now=%llu n=%zu",
                     slotRegion, static_cast<unsigned long long>(UnpackEpoch(epochPacked)),
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
