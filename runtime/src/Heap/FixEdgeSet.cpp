// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FixEdgeSet.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/ForwardFactTable.h"
#include "Heap/Heap.h"
#include "Heap/RelocationDiagnosticTable.h"
#include "Mutator/MutatorManager.h"

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

    // I5: target already From or GhostFrom → always register (bulk fix needs it).
    // plainsrc P11 (Idle plain→then-from) is covered by Trace I4 complement when
    // the holder is scanned; unreached holders need compiler dual-track (r1cc).
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
    // R2.2: entries are bare slot addresses with no self-bound validity; the whole
    // design leans on the [register, VisitAndClear] window closing inside one major's
    // BulkForward STW. That precondition was prose until now — make it a checked one.
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(),
               "FixEdgeSet slots are bare addresses and may only be consumed inside STW");
    struct CopyDstTypeCount {
        size_t fact{ 0 };
        size_t staleTarget{ 0 };
    };

    std::vector<MAddress> local;
    {
        std::lock_guard<std::mutex> lg(mutex);
        local.swap(slots);
        count.store(0, std::memory_order_relaxed);
    }
    copyDstFactCount.store(0, std::memory_order_relaxed);
    copyDstNoFactCount.store(0, std::memory_order_relaxed);
    copyDstStaleTargetCount.store(0, std::memory_order_relaxed);
    copyDstConstDomainFactCount.store(0, std::memory_order_relaxed);
    copyDstConstDomainStaleTargetCount.store(0, std::memory_order_relaxed);
    copyDstConstPoolDomainFactCount.store(0, std::memory_order_relaxed);
    copyDstConstPoolDomainStaleTargetCount.store(0, std::memory_order_relaxed);
    if (local.empty()) {
        return;
    }
    std::sort(local.begin(), local.end());
    local.erase(std::unique(local.begin(), local.end()), local.end());
    std::map<TypeInfo*, CopyDstTypeCount> copyDstTypeCounts;
    const size_t gcOrdinal = g_gcCount + 1;
    size_t copyDstTaggedStaleTargetCount = 0;
    size_t copyDstPlainStaleTargetCount = 0;
    for (MAddress addr : local) {
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            continue;
        }
        // Slot must sit in a non-ghost region (holder not evacuated).
        RegionInfo* slotRegion = RegionInfo::TryGetRegionInfoAt(static_cast<uintptr_t>(addr));
        if (slotRegion == nullptr) {
            e9GateSkipCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (slotRegion->IsGhostFromRegion() || slotRegion->IsFromRegion()) {
            BaseObject* toSlot = nullptr;
            size_t offset = 0;
            if (!ForwardFactTable::Instance().LookupContaining(reinterpret_cast<BaseObject*>(addr), toSlot, offset)) {
                copyDstNoFactCount.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<size_t> noFactSample{ 0 };
                const size_t n = noFactSample.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    VLOG(REPORT, "[COPY_DST_NO_FACT] from_slot=%p gc_ordinal=%zu n=%zu",
                         reinterpret_cast<void*>(addr), gcOrdinal, n);
                }
                e9GateSkipCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // The fix-set indexes a logical edge: when its holder moved, consume
            // that edge at the same offset in the completed destination copy.
            visitor(*reinterpret_cast<RefField<>*>(toSlot));
            BaseObject* fromHolder = reinterpret_cast<BaseObject*>(addr - offset);
            BaseObject* toHolder = reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(toSlot) - offset);
            RelocationDiagnosticTable::Entry relocation{ false, nullptr, nullptr, 0, nullptr };
            size_t diagnosticOffset = 0;
            TypeInfo* holderType = nullptr;
            if (RelocationDiagnosticTable::Instance().LookupContaining(
                    reinterpret_cast<BaseObject*>(addr), relocation, diagnosticOffset) &&
                relocation.from == fromHolder && relocation.to == toHolder && diagnosticOffset == offset) {
                holderType = relocation.typeInfo;
            }
            const char* holderTypeName = holderType == nullptr ? "<unknown>" : holderType->GetName();
            if (holderTypeName == nullptr) {
                holderTypeName = "<null-name>";
            }

            RefField<> toField(*reinterpret_cast<RefField<>*>(toSlot));
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
                         reinterpret_cast<void*>(addr), toSlot, offset, fromHolder, toHolder, holderType,
                         holderTypeName, target, gcOrdinal, staleN);
                }
            }
            static std::atomic<size_t> factSample{ 0 };
            const size_t factN = factSample.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((factN & (factN - 1)) == 0) {
                VLOG(REPORT,
                     "[COPY_DST_FACT] from_slot=%p to_slot=%p offset=%zu from_holder=%p to_holder=%p "
                     "holder_type_info=%p holder_type=%s target=%p stale_target=%d gc_ordinal=%zu n=%zu",
                     reinterpret_cast<void*>(addr), toSlot, offset, fromHolder, toHolder, holderType, holderTypeName,
                     target, static_cast<int>(staleTarget), gcOrdinal, factN);
            }
            e9GateSkipCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (slotRegion->IsFreeRegion() || slotRegion->IsGarbageRegion()) {
            e9GateSkipCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // P-G: touch only indexed field addresses — no object walk / GetSize.
        auto* field = reinterpret_cast<RefField<>*>(addr);
        visitor(*field);
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
}
} // namespace MapleRuntime
