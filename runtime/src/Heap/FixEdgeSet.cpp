// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FixEdgeSet.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
// VisitAndClear calls this only after proving that the carrier still names an
// allocated object in the same valid, non-source region identity.
ALWAYS_INLINE inline CurrentPtr ProvenByCarrierChecks(BaseObject* object) { return CurrentPtr(object); }

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
    staticSlots.push_back(slotAddr);
    count.fetch_add(1, std::memory_order_relaxed);
}

void FixEdgeSet::AddHolder(MAddress holder, size_t holderSize, size_t fieldOffset, uint64_t regionIdentityEpoch)
{
    std::lock_guard<std::mutex> lg(mutex);
    auto insertion = holders.emplace(holder, HolderEntry{ holderSize, regionIdentityEpoch, {} });
    auto it = insertion.first;
    bool inserted = insertion.second;
    CHECK_DETAIL(inserted || (it->second.holderSize == holderSize &&
                                 it->second.regionIdentityEpoch == regionIdentityEpoch),
                 "FixEdgeSet holder identity changed without invalidation holder=%p old_size=%zu new_size=%zu "
                 "old_epoch=%llu new_epoch=%llu",
                 reinterpret_cast<void*>(holder), it->second.holderSize, holderSize,
                 static_cast<unsigned long long>(it->second.regionIdentityEpoch),
                 static_cast<unsigned long long>(regionIdentityEpoch));
    it->second.fieldOffsets.push_back(fieldOffset);
    count.fetch_add(1, std::memory_order_relaxed);
    heapSlotCount.fetch_add(1, std::memory_order_release);
}

void FixEdgeSet::MaybeAdd(BaseObject* holder, RefField<>* slot, BaseObject* newRef)
{
    if (slot == nullptr || newRef == nullptr) {
        return;
    }
    if (!Heap::IsHeapAddress(newRef)) {
        return;
    }
    // A holder already in from/ghost is too late to register against that source
    // identity. Entries registered before the transition are relocated through
    // CopyObject; static roots use the separate absolute-address collection.
    RegionInfo* holderRegion = nullptr;
    MAddress holderAddress = 0;
    size_t holderSize = 0;
    size_t fieldOffset = 0;
    if (holder != nullptr) {
        if (!Heap::IsHeapAddress(holder)) {
            return;
        }
        if (RegionInfo::InGhostFromRegion(holder)) {
            return;
        }
        holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
        if (holderRegion == nullptr || holderRegion->IsFromRegion()) {
            return;
        }
        holderAddress = reinterpret_cast<MAddress>(holder);
        holderSize = RegionSpace::GetAllocSize(UnsafeAssumeCurrent(holder));
        MAddress slotAddress = reinterpret_cast<MAddress>(slot);
        CHECK_DETAIL(slotAddress >= holderAddress && slotAddress + sizeof(RefField<>) <= holderAddress + holderSize,
                     "FixEdgeSet field is outside holder holder=%p size=%zu field=%p",
                     holder, holderSize, slot);
        fieldOffset = slotAddress - holderAddress;
    }

    bool shouldAdd = false;
    // I5: target already From or GhostFrom → always register (bulk fix needs it).
    // plainsrc P11 (Idle plain→then-from) is covered by Trace I4 complement when
    // the holder is scanned; unreached holders need compiler dual-track (r1cc).
    if (RegionInfo::InGhostFromRegion(newRef)) {
        fromTargetRegistered.fetch_add(1, std::memory_order_relaxed);
        shouldAdd = true;
    }
    RegionInfo* targetRegion = shouldAdd ? nullptr :
        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(newRef));
    if (!shouldAdd && targetRegion != nullptr && targetRegion->IsFromRegion()) {
        fromTargetRegistered.fetch_add(1, std::memory_order_relaxed);
        shouldAdd = true;
    }
    // E6 (i)-narrow: only while GC phase is active (phase > INIT ⇒ mutator already
    // on slow-path barriers). Idle keeps I5-only (no FixSet inflation). Register
    // every cross-region heap ref store so live holders that later miss I4 still
    // enter the set (closes T1 black-allocation / concurrent-phase escape).
    if (!shouldAdd) {
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        if (phase <= GCPhase::GC_PHASE_INIT || holder == nullptr || targetRegion == nullptr ||
            holderRegion == targetRegion) {
            return;
        }
        crossRegionRegistered.fetch_add(1, std::memory_order_relaxed);
        shouldAdd = true;
    }
    if (holder == nullptr) {
        Add(reinterpret_cast<MAddress>(slot));
    } else {
        AddHolder(holderAddress, holderSize, fieldOffset, holderRegion->GetIdentityEpoch());
    }
}

size_t FixEdgeSet::InvalidateRangeLocked(MAddress start, size_t size)
{
    if (size == 0) {
        return 0;
    }
    CHECK_DETAIL(start + size >= start, "FixEdgeSet invalidation range overflow start=%p size=%zu",
                 reinterpret_cast<void*>(start), size);
    MAddress end = start + size;
    auto it = holders.lower_bound(start);
    if (it != holders.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second.holderSize > start) {
            it = previous;
        }
    }
    size_t removed = 0;
    while (it != holders.end() && it->first < end) {
        MAddress holderEnd = it->first + it->second.holderSize;
        if (holderEnd <= start) {
            ++it;
            continue;
        }
        removed += it->second.fieldOffsets.size();
        it = holders.erase(it);
    }
    return removed;
}

void FixEdgeSet::RelocateHolder(MAddress from, MAddress to, size_t size)
{
    if (from == to || size == 0 || heapSlotCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    HolderEntry moved{ 0, 0, {} };
    bool hasMovedCarrier = false;
    auto source = holders.find(from);
    if (source != holders.end()) {
        moved = std::move(source->second);
        holders.erase(source);
        hasMovedCarrier = true;
        CHECK_DETAIL(moved.holderSize == size,
                     "FixEdgeSet copy size differs from holder carrier holder=%p carrier=%zu copy=%zu",
                     reinterpret_cast<void*>(from), moved.holderSize, size);
    }
    size_t removed = InvalidateRangeLocked(to, size);
    if (removed != 0) {
        count.fetch_sub(removed, std::memory_order_relaxed);
        heapSlotCount.fetch_sub(removed, std::memory_order_release);
        invalidatedSlots.fetch_add(removed, std::memory_order_relaxed);
    }
    if (!hasMovedCarrier) {
        return;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(to);
    CHECK_DETAIL(toRegion != nullptr && toRegion->IsValidRegion() && !toRegion->IsFreeRegion() &&
                     !toRegion->IsGarbageRegion(),
                 "FixEdgeSet relocation destination has no live region from=%p to=%p size=%zu",
                 reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
    moved.regionIdentityEpoch = toRegion->GetIdentityEpoch();
    moved.relocationSource = from;
    size_t movedSlotCount = moved.fieldOffsets.size();
    auto insertion = holders.emplace(to, std::move(moved));
    CHECK_DETAIL(insertion.second, "FixEdgeSet relocation destination carrier remained to=%p",
                 reinterpret_cast<void*>(to));
    relocatedSlots.fetch_add(movedSlotCount, std::memory_order_relaxed);
}

void FixEdgeSet::InvalidateRange(MAddress start, size_t size)
{
    if (size == 0 || heapSlotCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    size_t removed = InvalidateRangeLocked(start, size);
    if (removed != 0) {
        count.fetch_sub(removed, std::memory_order_relaxed);
        heapSlotCount.fetch_sub(removed, std::memory_order_release);
        invalidatedSlots.fetch_add(removed, std::memory_order_relaxed);
    }
}

void FixEdgeSet::VisitAndClear(const SlotVisitor& visitor)
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(),
               "FixEdgeSet holder carriers may only be consumed inside STW");
    struct CopyDstTypeCount {
        size_t fact{ 0 };
        size_t staleTarget{ 0 };
    };

    std::map<MAddress, HolderEntry> localHolders;
    std::vector<MAddress> localStaticSlots;
    {
        std::lock_guard<std::mutex> lg(mutex);
        localHolders.swap(holders);
        localStaticSlots.swap(staticSlots);
        count.store(0, std::memory_order_relaxed);
        heapSlotCount.store(0, std::memory_order_release);
    }
    copyDstFactCount.store(0, std::memory_order_relaxed);
    // This is structurally zero after synchronous carrier rebasing. It is not a
    // health signal; carrier-scope validation below is the fail-stop guard.
    copyDstNoFactCount.store(0, std::memory_order_relaxed);
    copyDstStaleTargetCount.store(0, std::memory_order_relaxed);
    copyDstConstDomainFactCount.store(0, std::memory_order_relaxed);
    copyDstConstDomainStaleTargetCount.store(0, std::memory_order_relaxed);
    copyDstConstPoolDomainFactCount.store(0, std::memory_order_relaxed);
    copyDstConstPoolDomainStaleTargetCount.store(0, std::memory_order_relaxed);

    std::sort(localStaticSlots.begin(), localStaticSlots.end());
    localStaticSlots.erase(std::unique(localStaticSlots.begin(), localStaticSlots.end()), localStaticSlots.end());
    for (MAddress addr : localStaticSlots) {
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            continue;
        }
        auto* field = reinterpret_cast<RefField<>*>(addr);
        visitor(nullptr, *field);
    }

    std::map<TypeInfo*, CopyDstTypeCount> copyDstTypeCounts;
    const size_t gcOrdinal = g_gcCount + 1;
    size_t copyDstTaggedStaleTargetCount = 0;
    size_t copyDstPlainStaleTargetCount = 0;
    size_t visitedSlots = 0;
    bool escapePositiveControlInjected = false;
    const char* escapePositiveControl = std::getenv("MRT_FIX_EDGE_SET_ESCAPE_POSCTRL");
    for (auto& holderAndEntry : localHolders) {
        MAddress holderAddress = holderAndEntry.first;
        HolderEntry& entry = holderAndEntry.second;
        RegionInfo* holderRegion = Heap::IsHeapAddress(holderAddress) ?
            RegionInfo::TryGetRegionInfoAt(holderAddress) : nullptr;
        if (!escapePositiveControlInjected && escapePositiveControl != nullptr &&
            std::strcmp(escapePositiveControl, "1") == 0 && holderRegion != nullptr) {
            entry.regionIdentityEpoch = holderRegion->GetIdentityEpoch() ^ 1;
            escapePositiveControlInjected = true;
            VLOG(REPORT, "[FixEdgeSetCarrier] escape_positive_control holder=%p",
                 reinterpret_cast<void*>(holderAddress));
        }
        CHECK_DETAIL(holderRegion != nullptr && holderRegion->IsValidRegion() &&
                         !holderRegion->IsGhostFromRegion() && !holderRegion->IsFromRegion() &&
                         !holderRegion->IsFreeRegion() && !holderRegion->IsGarbageRegion() &&
                         holderRegion->GetIdentityEpoch() == entry.regionIdentityEpoch,
                     "FixEdgeSet holder carrier escaped its validity scope holder=%p size=%zu epoch=%llu region=%p",
                     reinterpret_cast<void*>(holderAddress), entry.holderSize,
                     static_cast<unsigned long long>(entry.regionIdentityEpoch), holderRegion);
        CHECK_DETAIL(holderAddress >= holderRegion->GetRegionStart() &&
                         holderAddress + entry.holderSize <= holderRegion->GetRegionAllocPtr(),
                     "FixEdgeSet holder carrier outside allocation frontier holder=%p size=%zu region=%p",
                     reinterpret_cast<void*>(holderAddress), entry.holderSize, holderRegion);
        BaseObject* holder = reinterpret_cast<BaseObject*>(holderAddress);
        CurrentPtr currentHolder = ProvenByCarrierChecks(holder);
        CHECK_DETAIL(RegionSpace::GetAllocSize(currentHolder) == entry.holderSize,
                     "FixEdgeSet holder size changed without relocation holder=%p expected=%zu actual=%zu",
                     holder, entry.holderSize, RegionSpace::GetAllocSize(currentHolder));
        std::sort(entry.fieldOffsets.begin(), entry.fieldOffsets.end());
        entry.fieldOffsets.erase(std::unique(entry.fieldOffsets.begin(), entry.fieldOffsets.end()),
                                 entry.fieldOffsets.end());
        const bool wasRelocated = entry.relocationSource != 0;
        TypeInfo* holderType = wasRelocated ? GetTypeInfo(currentHolder) : nullptr;
        for (size_t offset : entry.fieldOffsets) {
            CHECK_DETAIL(offset + sizeof(RefField<>) <= entry.holderSize,
                         "FixEdgeSet field offset outside holder holder=%p size=%zu offset=%zu",
                         holder, entry.holderSize, offset);
            auto* field = reinterpret_cast<RefField<>*>(holderAddress + offset);
            visitor(holder, *field);
            ++visitedSlots;
            if (!wasRelocated) {
                continue;
            }

            const char* holderTypeName = holderType == nullptr ? "<unknown>" : holderType->GetName();
            if (holderTypeName == nullptr) {
                holderTypeName = "<null-name>";
            }

            RefField<> toField(*field);
            BaseObject* target = toField.GetTargetObject();
            bool staleTarget = false;
            if (Heap::IsHeapAddress(target)) {
                staleTarget = RegionInfo::InGhostFromRegion(target);
                if (!staleTarget) {
                    RegionInfo* targetRegion =
                        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
                    staleTarget = targetRegion != nullptr && targetRegion->IsFromRegion();
                }
            }

            copyDstFactCount.fetch_add(1, std::memory_order_relaxed);
            CopyDstTypeCount& typeCount = copyDstTypeCounts[holderType];
            ++typeCount.fact;
            const bool isConstPoolDomainEntries = std::strstr(holderTypeName, "RawArray") != nullptr &&
                std::strstr(holderTypeName, "HashMapEntry") != nullptr &&
                std::strstr(holderTypeName, "ConstPoolDomain") != nullptr;
            const bool isConstDomainEntries = !isConstPoolDomainEntries &&
                std::strstr(holderTypeName, "RawArray") != nullptr &&
                std::strstr(holderTypeName, "HashMapEntry") != nullptr &&
                std::strstr(holderTypeName, "ConstDomain") != nullptr;
            if (isConstDomainEntries) {
                copyDstConstDomainFactCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (isConstPoolDomainEntries) {
                copyDstConstPoolDomainFactCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (staleTarget) {
                copyDstStaleTargetCount.fetch_add(1, std::memory_order_relaxed);
                if (toField.IsTagged()) {
                    ++copyDstTaggedStaleTargetCount;
                } else {
                    ++copyDstPlainStaleTargetCount;
                }
                ++typeCount.staleTarget;
                if (isConstDomainEntries) {
                    copyDstConstDomainStaleTargetCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (isConstPoolDomainEntries) {
                    copyDstConstPoolDomainStaleTargetCount.fetch_add(1, std::memory_order_relaxed);
                }
                static std::atomic<size_t> staleSample{ 0 };
                const size_t staleN = staleSample.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((staleN & (staleN - 1)) == 0) {
                    VLOG(REPORT,
                         "[COPY_DST_STALE_TARGET] from_slot=%p to_slot=%p offset=%zu from_holder=%p "
                         "to_holder=%p holder_type_info=%p holder_type=%s target=%p gc_ordinal=%zu n=%zu",
                         reinterpret_cast<void*>(entry.relocationSource + offset), field, offset,
                         reinterpret_cast<void*>(entry.relocationSource), holder, holderType, holderTypeName, target,
                         gcOrdinal, staleN);
                }
            }
            static std::atomic<size_t> factSample{ 0 };
            const size_t factN = factSample.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((factN & (factN - 1)) == 0) {
                VLOG(REPORT,
                     "[COPY_DST_FACT] from_slot=%p to_slot=%p offset=%zu from_holder=%p to_holder=%p "
                     "holder_type_info=%p holder_type=%s target=%p stale_target=%d gc_ordinal=%zu n=%zu",
                     reinterpret_cast<void*>(entry.relocationSource + offset), field, offset,
                     reinterpret_cast<void*>(entry.relocationSource), holder, holderType, holderTypeName, target,
                     static_cast<int>(staleTarget), gcOrdinal, factN);
            }
        }
    }
    for (const auto& typeCount : copyDstTypeCounts) {
        const char* typeName = typeCount.first == nullptr ? "<unknown>" : typeCount.first->GetName();
        VLOG(REPORT,
             "[COPY_DST_TYPE] holder_type_info=%p holder_type=%s fact=%zu stale_target=%zu gc_ordinal=%zu",
             typeCount.first, typeName == nullptr ? "<null-name>" : typeName, typeCount.second.fact,
             typeCount.second.staleTarget, gcOrdinal);
    }
    VLOG(REPORT, "[COPY_DST_STALE_TAG_SPLIT] tagged=%zu plain=%zu gc_ordinal=%zu",
         copyDstTaggedStaleTargetCount, copyDstPlainStaleTargetCount, gcOrdinal);
    VLOG(REPORT, "[COPY_DST_NO_FACT] structural_zero=1 health_signal=0 escaped_carrier_guard=fail-stop");
    VLOG(REPORT, "[FixEdgeSetCarrier] holders=%zu visited=%zu relocated=%zu invalidated=%zu static=%zu",
         localHolders.size(), visitedSlots, relocatedSlots.exchange(0, std::memory_order_relaxed),
         invalidatedSlots.exchange(0, std::memory_order_relaxed), localStaticSlots.size());
}
} // namespace MapleRuntime
