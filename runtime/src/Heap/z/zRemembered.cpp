// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
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
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
namespace WCollectorInternal {
// ZGC zPage.inline.hpp:254-256: is_object_live = is_allocating || livemap.
// zBarrier.inline.hpp:73-78: never heal a non-null slot with null.
// 4fcf746a used IsMarkedObject<Old> only — post-flip to-space and young
// holders have no Old face, so F3 / Resolve / Scrub planted null into live
// Array slots (nwreclaim: pc_off=0x29589 mov 0x8(%rcx) rcx=0).
bool HolderObjectIsLive(BaseObject* holder)
{
    if (holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->IsValidObject()) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(holder));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (region->IsAllocating()) {
        return true;
    }
    return region->is_object_strongly_live(from_object(holder));
}

bool SlotHeldByLiveObject(const void* slot)
{
    if (slot == nullptr || !Heap::IsHeapAddress(slot)) {
        return false;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(slot));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    if (region->IsAllocating()) {
        return true;
    }
    // zRememberedSet.cpp:144-152 / zPage.inline.hpp:371-386 find_base.
    const MAddress base = region->find_base_unsafe(reinterpret_cast<MAddress>(slot));
    BaseObject* holder = base == 0 ? nullptr : from_region_addr(base);
    if (holder == nullptr || reinterpret_cast<MAddress>(slot) - reinterpret_cast<MAddress>(holder) >=
        RegionSpace::GetAllocSize(*holder)) {
        return false;
    }
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
            ZPage* page = forwarding->page();
            remset.VisitPreviousInRange(forwarding->start(), forwarding->size(), [&](MAddress field) {
                if (page == nullptr) {
                    return;
                }
                const MAddress addr = page->find_base_unsafe(field);
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
        ZVerify::AfterScan(forwarding);
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
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(target));
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
