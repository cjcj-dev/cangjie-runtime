// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/CollectorResources.h"
#include <unordered_set>
#include <vector>
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
// zForwarding.cpp:369-409. Inspect the real table, including the port's
// overflow entries; never reconstruct mappings from object headers.
void ZForwarding::verify() const
{
    CHECK_DETAIL(_ref_count.load(std::memory_order_acquire) != 0, "Invalid forwarding reference count");
    CHECK_DETAIL(_page != nullptr, "Invalid forwarding page");
    std::vector<MAddress> sources;
    for_each_from([&](MAddress from) { sources.push_back(from); });
    std::unordered_set<MAddress> uniqueSources;
    std::unordered_set<MAddress> destinations;
    size_t bytes = 0;
    for (MAddress from : sources) {
        CHECK_DETAIL(from >= start() && from - start() < size(), "Invalid forwarding source");
        CHECK_DETAIL(uniqueSources.insert(from).second, "Duplicate forwarding source");
        const MAddress to = find(from);
        CHECK_DETAIL(to != 0 && destinations.insert(to).second, "Duplicate or null forwarding destination");
        BaseObject* object = reinterpret_cast<BaseObject*>(to);
        ZVerify::Object(object, nullptr);
        bytes += RegionSpace::GetAllocSize(*object);
    }
    // The source incarnation's livemap is retained by FromPageView even for
    // in-place relocation, where reusable page metadata already names to-space.
    const FromPageView* from = from_page_snapshot();
    CHECK_DETAIL(from != nullptr && from->liveInfo != nullptr, "Missing forwarding source livemap");
    RegionBitmap* bitmap = _page->GetOwnerMarkBitmap(from->liveInfo);
    CHECK_DETAIL(bitmap != nullptr && sources.size() == bitmap->GetLiveObjects() &&
                 bytes == bitmap->GetLiveBytes(), "Invalid forwarding live objects or bytes");
}

namespace {
// zVerify.cpp:521-523. Replaced only at a color-flip safepoint.
std::unordered_set<MAddress> bufferedStores;
bool IntentionallyUnremembered(zpointer value)
{
    // The upstream exemption is both remembered bits, not just the current bit.
    return (raw(value) & REMEMBERED_MASK) == REMEMBERED_MASK;
}
}
void ZVerify::OnColorFlip()
{
    if (!ZVerifyRemembered || !kBufferStoreBarriers) { return; }
    bufferedStores.clear();
    MutatorManager::Instance().VisitStoreBarrierBuffers([](MAddress slot) { bufferedStores.insert(slot); });
}

// zVerify.cpp:531-609. Source-page verification is old-to-old only.
void ZVerify::BeforeRelocation(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr ||
        forwarding->table_generation() != static_cast<uint8_t>(Generation::Old)) { return; }
    RegionInfo* page = forwarding->page();
    if (page == nullptr) { return; }
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    const bool activeCurrent = Heap::GetHeap().GetCollector().OldActiveRemsetIsCurrent();
    CHECK_DETAIL(remset.IsClearInRange(forwarding->start(), forwarding->size(), !activeCurrent),
                 "Inactive remembered set is not empty for %p", page);
    page->VisitLiveObjectsUntilFalse([&](BaseObject* object) {
        const MAddress from = reinterpret_cast<MAddress>(object);
        HeapIterator::Fields(object, true, [&](BaseObject*, RefField<>& field) {
            const MAddress slot = reinterpret_cast<MAddress>(&field);
            if (IntentionallyUnremembered(field.GetFieldValue()) || bufferedStores.count(slot) != 0 ||
                forwarding->find(from) != 0) { return; }
            CHECK_DETAIL(activeCurrent ? remset.Contains(slot) : remset.ContainsPrevious(slot),
                         "Missing remembered field %p in source %p", &field, object);
        });
        return true;
    });
}

// zVerify.cpp:610-738. Recheck the pointer after reading both bitmap faces;
// a concurrent scanner may have self-healed the pointer and cleared the bit.
void ZVerify::AfterRelocationInternal(ZForwarding* forwarding)
{
    std::vector<MAddress> fromAddresses;
    forwarding->for_each_from([&](MAddress from) { fromAddresses.push_back(from); });
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    for (MAddress from : fromAddresses) {
        BaseObject* object = reinterpret_cast<BaseObject*>(forwarding->find(from));
        Object(object, nullptr);
        // Destination age is represented by the destination page in this port.
        if (RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object))->IsYoungRegion()) { continue; }
        HeapIterator::Fields(object, true, [&](BaseObject*, RefField<>& field) {
            const MAddress slot = reinterpret_cast<MAddress>(&field);
            const zpointer value = field.GetFieldValue(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);
            RefField<> preloaded(value);
            if (IntentionallyUnremembered(value) || Heap::GetHeap().GetCollector().is_store_good(preloaded) ||
                bufferedStores.count(slot) != 0 ||
                bufferedStores.count(from + slot - reinterpret_cast<MAddress>(object)) != 0) { return; }
            if (remset.Contains(slot) || remset.ContainsPrevious(slot)) { return; }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (field.GetFieldValue(std::memory_order_acquire) != value) { return; }
            CHECK_DETAIL(ZForwarding::young_marking(), "Missing remembered field outside young marking: %p", &field);
            CHECK_DETAIL(forwarding->relocated_remembered_fields_published_contains(slot),
                         "Missing published remembered field %p in destination %p", &field, object);
        });
    }
}
void ZVerify::AfterRelocation(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr) { return; }
    if (ZForwarding::young_marking() && forwarding->relocated_remembered_fields_is_concurrently_scanned()) { return; }
    AfterRelocationInternal(forwarding);
}
void ZVerify::AfterScan(ZForwarding* forwarding)
{
    if (!ZVerifyRemembered || forwarding == nullptr ||
        Heap::GetHeap().GetCollectorResources().GetYoungDriverPort().Abort().IsRequested()) { return; }
    const auto phase = Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::OLD).phase;
    if ((phase != GC_PHASE_FORWARD && phase != GC_PHASE_PREFORWARD) ||
        !forwarding->relocated_remembered_fields_is_concurrently_scanned()) { return; }
    AfterRelocationInternal(forwarding);
}
} // namespace MapleRuntime
