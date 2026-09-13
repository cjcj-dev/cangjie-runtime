// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/WCollector/WCollector.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "Concurrency/Concurrency.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Collector/GcTriggerFlags.h"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/ZForwarding.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Common/ColourPredicates.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Verify/VerifyRegions.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
struct RemsetFilterReceiptState {
    std::atomic<uint64_t> seen { 0 };
    std::atomic<uint64_t> consumed { 0 };
    std::atomic<uint64_t> stale { 0 };
    std::atomic<uint64_t> deadHolder { 0 };
    std::atomic<uint64_t> noOrigin { 0 };
    std::atomic<uint64_t> badTarget { 0 };
    std::atomic<MAddress> lastConsumedSlot { 0 };
    std::atomic<MAddress> lastStaleSlot { 0 };
    std::atomic<MAddress> lastDeadHolderSlot { 0 };
    std::atomic<MAddress> lastNoOriginSlot { 0 };
    std::atomic<MAddress> lastBadTargetSlot { 0 };
};
RemsetFilterReceiptState g_remsetFilterReceipt;
}

void ResetRemsetFilterTestReceipt()
{
    g_remsetFilterReceipt.seen.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.consumed.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.stale.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.deadHolder.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.noOrigin.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.badTarget.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastConsumedSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastStaleSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastDeadHolderSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastNoOriginSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastBadTargetSlot.store(0, std::memory_order_relaxed);
}

RemsetFilterTestReceipt ReadRemsetFilterTestReceipt()
{
    return { g_remsetFilterReceipt.seen.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.consumed.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.stale.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.deadHolder.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.noOrigin.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.badTarget.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastConsumedSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastStaleSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastDeadHolderSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastNoOriginSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastBadTargetSlot.load(std::memory_order_relaxed) };
}

void NoteRemsetFilterTestReceipt(MAddress slot, RemsetFilterReceiptReason reason, bool consumed)
{
    if (slot == 0) {
        return;
    }
    g_remsetFilterReceipt.seen.fetch_add(1, std::memory_order_relaxed);
    if (consumed) {
        g_remsetFilterReceipt.consumed.fetch_add(1, std::memory_order_relaxed);
        g_remsetFilterReceipt.lastConsumedSlot.store(slot, std::memory_order_relaxed);
    }
    switch (reason) {
        case RemsetFilterReceiptReason::kStale:
            g_remsetFilterReceipt.stale.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastStaleSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kDeadHolder:
            g_remsetFilterReceipt.deadHolder.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastDeadHolderSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNoOrigin:
            g_remsetFilterReceipt.noOrigin.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastNoOriginSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kBadTarget:
            g_remsetFilterReceipt.badTarget.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastBadTargetSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNone:
            break;
    }
}
#endif
#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
namespace WCollectorInternal {
// ZGC zPage.inline.hpp:254-256: is_object_live = is_allocating || livemap.
// zBarrier.inline.hpp:73-78: never heal a non-null slot with null.
// 4fcf746a used IsMarkedObject<Old> only — post-flip to-space and young
// holders have no Old face, so F3 / Resolve / Scrub planted null into live
// Array slots (nwreclaim: pc_off=0x29589 mov 0x8(%rcx) rcx=0).
bool RegionIsAllocatingPage(const RegionInfo* region)
{
    if (region == nullptr) {
        return false;
    }
    const RegionInfo::RegionType type = region->GetRegionType();
    return region->IsToRegion() || region->IsThreadLocalRegion() ||
        type == RegionInfo::RegionType::RECENT_FULL_REGION ||
        type == RegionInfo::RegionType::RECENT_LARGE_REGION ||
        type == RegionInfo::RegionType::TL_RAW_POINTER_REGION ||
        type == RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION ||
        region->IsPinnedRegion() || region->HasMarkStartAllocGap();
}

bool HolderObjectIsLive(BaseObject* holder)
{
    if (holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->IsValidObject()) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (RegionIsAllocatingPage(region)) {
        return true;
    }
    if (region->IsYoungRegion()) {
        return RegionSpace::IsMarkedObject<Generation::Young>(holder);
    }
    return RegionSpace::IsMarkedObject<Generation::Old>(holder);
}

bool SlotHeldByLiveObject(const void* slot)
{
    if (slot == nullptr || !Heap::IsHeapAddress(slot)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(slot));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (RegionIsAllocatingPage(region)) {
        return true;
    }
    BaseObject* holder = Collector::TryRecoverInteriorBase(
        reinterpret_cast<BaseObject*>(const_cast<void*>(slot)));
    return HolderObjectIsLive(holder);
}

} // namespace WCollectorInternal
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
void WCollector::ScanRelocatedRememberedFields(MinorSlotSet& rememberedSlots)
{
    struct Containing {
        MAddress addr;
        MAddress field;
    };
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    ForwardingTable::VisitAll(Generation::Old, [&](ZForwarding* forwarding) {
        if (forwarding == nullptr) {
            return;
        }
        if (forwarding->retain_page()) {
            forwarding->relocated_remembered_fields_notify_concurrent_scan_of();
            std::vector<Containing> containing;
            RegionInfo* page = forwarding->page();
            remset.VisitPreviousInRange(forwarding->start(), forwarding->size(), [&](MAddress field) {
                if (page == nullptr) {
                    return;
                }
                const MAddress addr = page->FindLiveObjectStart(field);
                if (addr == 0 || addr > field) {
                    return;
                }
                containing.push_back(Containing{ addr, field });
            });
            forwarding->release_page();
            MAddress cachedFrom = 0;
            MAddress cachedTo = 0;
            size_t cachedSize = 0;
            for (const Containing& entry : containing) {
                if (entry.addr != cachedFrom) {
                    cachedFrom = entry.addr;
                    BaseObject* from = reinterpret_cast<BaseObject*>(entry.addr);
                    BaseObject* to = relocate_or_remap_object(from, ZGenerationId::old);
                    CHECK_DETAIL(to != nullptr, "remembered containing object must be relocated");
                    cachedTo = reinterpret_cast<MAddress>(to);
                    cachedSize = RegionSpace::GetAllocSize(*to);
                }
                const uintptr_t fieldOffset = entry.field - entry.addr;
                if (fieldOffset < cachedSize) {
                    rememberedSlots.insert(cachedTo + fieldOffset);
                }
            }
        } else {
            // ref == 0 releases source bytes before PageWorkScope marks done.
            // Consume the published fields only after that same page task completes.
            ZForwardingLife::WaitPageDone(forwarding);
            CHECK(forwarding->is_done());
            forwarding->relocated_remembered_fields_apply_to_published([&](MAddress field) {
                rememberedSlots.insert(field);
            });
        }
    });
}

void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     const MinorObjectSet& currentMinorRoots, bool fullYoungScan,
                                      MinorSlotSet* consumedOut, RemsetScanStats* statsOut,
                                      MinorInteriorBaseMap* interiorBasesOut, const ScopedStopTheWorld* stw)
{
    (void)reachableSlots;
    (void)currentMinorRoots;
    (void)fullYoungScan;
    (void)interiorBasesOut;
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    // ZRemembered::scan_field (zRemembered.cpp:578-589): resolve/mark the
    // field, then rearm precisely when its healed target remains young.
    for (MAddress slot : rememberedSlots) {
        if (LedgerCount(weakSlots, slot) != 0) {
            // Weak fields continue in the reference-processing domain.
            remset.Record(slot);
            if (statsOut != nullptr) ++statsOut->skippedWeak;
            continue;
        }
        BaseObject* target = ResolveMinorReference(HeapSlotAt<>(slot), stw);
        if (target == nullptr || !Heap::IsHeapAddress(target)) continue;
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (!region->IsYoungRegion()) continue;
        PushYoungObject(target, workStack, "remset");
        remset.Record(slot);
        if (consumedOut != nullptr) consumedOut->insert(slot);
        if (statsOut != nullptr) ++statsOut->consumed;
#if defined(MRT_TESTABLE_INTERNALS)
        NoteRemsetFilterTestReceipt(slot, RemsetFilterReceiptReason::kNone, true);
#endif
    }
}
} // namespace MapleRuntime
