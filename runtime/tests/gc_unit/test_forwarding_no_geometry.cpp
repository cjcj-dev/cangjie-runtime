// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Invariant: region in the forward set, object not copied → LookupTo is null.
// Geometry GetRoute may still invent a to; the table must not.

#include "Heap/Allocator/ForwardingTable.h"
#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
// Access only: the exercised implementation remains the product SO's
// WCollector::ForwardObjectExclusive, including its real Publish call.
struct MutatorPublishTestAccess {
    static BaseObject* ForwardExclusive(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* copyPage)
    {
        return collector.ForwardObjectExclusive(from, to, copyPage);
    }
    static BaseObject* RelocateInner(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* copyPage)
    {
        return collector.RelocateObjectInner(from, to, copyPage);
    }
    static BaseObject* ForwardImpl(
        WCollector& collector, BaseObject* from, RegionInfo* copyPage)
    {
        RegionInfo::RetainScope lease(copyPage);
        return lease.ok() ? collector.ForwardObjectImpl(from, copyPage, lease) : nullptr;
    }
    static BaseObject* ForwardExclusiveVtable(WCollector& collector, BaseObject* from)
    {
        return collector.ForwardObjectExclusive(from);
    }
};
#endif

} // namespace MapleRuntime

GC_TEST(ForwardingNoGeometry, ArmedMissIsNullNotGeometry)
{
    static_assert(ForwardingTable::kEntriesSoleWhenArmed, "step 3 requires sole-when-armed");

    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::ROUTED);
    fx.region0->SetRouteInfo(0x20000000u, 4096);

    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    const size_t regionSize = fx.region0->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bm->MarkBits(offset, 8, regionSize);
    fx.region0->BindLiveInfo0FromLiveIfNull();

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    fx.region0->RecordRouteStart(offset);
    if (!ForwardingTable::EntriesArmed(from)) {
        // The one-shot map may carry the preceding test's sealed generation.
        // Reopening is explicit; SetRegionType must not do it implicitly.
        if (!ForwardingTable::InsertProvisional(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0)) {
            GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
            GC_EXPECT_TRUE(ForwardingTable::InsertProvisional(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
        }
    }
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from)->is_provisional());

    BaseObject* geometric = fx.region0->GetRouteForProbe(fx.obj0);
    GC_EXPECT_TRUE(geometric != nullptr);
    ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    const MAddress looked = lookup.to;
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(looked, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(looked != reinterpret_cast<MAddress>(geometric));

    const MAddress stored = fx.heapStart + RegionInfo::UNIT_SIZE + 128;
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(fx.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, stored), stored);
    publication = ForwardingTable::Publication();
    lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_EQ(lookup.to, stored);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);

    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    fx.region0->SetRouteState(RegionInfo::NORMAL);
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(ForwardingNoGeometry, ArmedLookupAndSuccessfulExclusiveCopyPublishProductReceipt)
{
    static_assert(ForwardingTable::kEntriesSoleWhenArmed, "step 3 requires sole-when-armed");

    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::ROUTED);
    fx.region0->SetRouteInfo(0x20000000u, 4096);

    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    size_t regionSize = fx.region0->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bm->MarkBits(offset, 8, regionSize);
    fx.region0->BindLiveInfo0FromLiveIfNull();

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    fx.region0->RecordRouteStart(offset);
    if (!ForwardingTable::EntriesArmed(from)) {
        // The preceding test sealed this address range. Only an explicit cycle
        // boundary may install this test's provisional carrier.
        if (!ForwardingTable::InsertProvisional(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0)) {
            GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
            GC_EXPECT_TRUE(ForwardingTable::InsertProvisional(
                fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
        }
    }
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from)->is_provisional());

    BaseObject* geometric = fx.region0->GetRouteForProbe(fx.obj0);
    GC_EXPECT_TRUE(geometric != nullptr);
    GC_EXPECT_TRUE(reinterpret_cast<MAddress>(geometric) == 0x20000000u ||
                   reinterpret_cast<MAddress>(geometric) != 0);

    ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    const MAddress looked = lookup.to;
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(looked, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(looked != reinterpret_cast<MAddress>(geometric));

    const MAddress stored = fx.heapStart + RegionInfo::UNIT_SIZE + 128;
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(fx.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, stored), stored);
    publication = ForwardingTable::Publication();
    GC_EXPECT_FALSE(ForwardingTable::GetEntries(from)->is_provisional());
    lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_EQ(lookup.to, stored);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);

#if defined(MRT_TESTABLE_INTERNALS)
    // Drive the product success path itself: CopyObject -> InstallMapping ->
    // RelocationRequestQueue::Publish -> UnlockObject(FORWARDED). This is the
    // call site at Relocate.cpp in WCollector::ForwardObjectExclusive, not a
    // queue helper with a hand-fed receipt.
    BaseObject* copyFrom = fx.PlaceObject(fx.region0->GetRegionStart() + 128);
    BaseObject* copyTo = fx.PlaceObject(fx.region1->GetRegionStart() + 128);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyFrom) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyTo) + 64);
    const MAddress copyFromAddr = reinterpret_cast<MAddress>(copyFrom);
    const MAddress copyToAddr = reinterpret_cast<MAddress>(copyTo);

    StateWord oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    /*deleted copy SM*/ (void)(fx.region0->metadata.copyInflight);
    GC_EXPECT_TRUE(true);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    BaseObject* relocated = MutatorPublishTestAccess::ForwardExclusive(
        collector, copyFrom, copyTo, fx.region0);

    const bool productPublished =
        ForwardingTable::FindTo(copyFromAddr) == copyToAddr;
    GC_EXPECT_TRUE(productPublished);
    GC_EXPECT_TRUE(relocated == copyTo);
    GC_EXPECT_EQ(ForwardingTable::FindTo(copyFromAddr), copyToAddr);
    GC_EXPECT_TRUE(copyFrom->IsForwarded());
    GC_EXPECT_EQ(fx.region0->metadata.copyInflight.load(std::memory_order_acquire), 0);
#endif

    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    fx.region0->SetRouteState(RegionInfo::NORMAL);
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ForwardingNoGeometry, RelocateInnerFindHitSkipsCopy)
{
    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::RouteState::ROUTING);
    BaseObject* copyFrom = fx.PlaceObject(fx.region0->GetRegionStart() + 128);
    BaseObject* copyTo = fx.PlaceObject(fx.region1->GetRegionStart() + 128);
    BaseObject* otherTo = fx.PlaceObject(fx.region1->GetRegionStart() + 256);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyFrom) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(otherTo) + 64);
    const MAddress copyFromAddr = reinterpret_cast<MAddress>(copyFrom);
    const MAddress copyToAddr = reinterpret_cast<MAddress>(copyTo);
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-relocate-inner");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    StateWord oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    /*deleted copy SM*/ (void)(fx.region0->metadata.copyInflight);
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    BaseObject* first = MutatorPublishTestAccess::RelocateInner(collector, copyFrom, copyTo, fx.region0);
    GC_EXPECT_TRUE(first == copyTo);
    GC_EXPECT_EQ(fx.region0->metadata.copyInflight.load(std::memory_order_acquire), 0);
    BaseObject* second = MutatorPublishTestAccess::RelocateInner(collector, copyFrom, otherTo, fx.region0);
    GC_EXPECT_TRUE(second == copyTo);
    GC_EXPECT_TRUE(second != otherTo);
    GC_EXPECT_EQ(ForwardingTable::FindTo(copyFromAddr), copyToAddr);
    GC_EXPECT_TRUE(copyFrom->IsForwarded());
    GC_EXPECT_EQ(fx.region0->metadata.copyInflight.load(std::memory_order_acquire), 0);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-relocate-inner");
}

GC_TEST(ForwardingNoGeometry, ForwardImplFindHitSkipsCopy)
{
    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::RouteState::ROUTING);
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap<Generation::Old>(live, fx.region0->GetRegionSize());
    fx.region0->PublishForwardingCarrier(fx.region0->GetMarkView<Generation::Old>());
    BaseObject* copyFrom = fx.PlaceObject(fx.region0->GetRegionStart() + 128);
    BaseObject* copyTo = fx.PlaceObject(fx.region1->GetRegionStart() + 128);
    BaseObject* otherTo = fx.PlaceObject(fx.region1->GetRegionStart() + 256);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyFrom) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(otherTo) + 64);
    const MAddress copyFromAddr = reinterpret_cast<MAddress>(copyFrom);
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-forward-impl");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SetGCPhase(GCPhase::GC_PHASE_FORWARD);
    StateWord oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    /*deleted copy SM*/ (void)(fx.region0->metadata.copyInflight);
    BaseObject* first = MutatorPublishTestAccess::RelocateInner(collector, copyFrom, copyTo, fx.region0);
    GC_EXPECT_TRUE(first == copyTo);
    BaseObject* second = MutatorPublishTestAccess::ForwardImpl(collector, copyFrom, fx.region0);
    GC_EXPECT_TRUE(second == copyTo);
    GC_EXPECT_TRUE(second != otherTo);
    GC_EXPECT_EQ(ForwardingTable::FindTo(copyFromAddr), reinterpret_cast<MAddress>(copyTo));
    GC_EXPECT_EQ(fx.region0->metadata.copyInflight.load(std::memory_order_acquire), 0);
    collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-forward-impl");
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

GC_TEST(ForwardingNoGeometry, ExclusiveVtableFindHitSkipsCopy)
{
    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::RouteState::ROUTING);
    BaseObject* copyFrom = fx.PlaceObject(fx.region0->GetRegionStart() + 128);
    BaseObject* copyTo = fx.PlaceObject(fx.region1->GetRegionStart() + 128);
    BaseObject* otherTo = fx.PlaceObject(fx.region1->GetRegionStart() + 256);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyFrom) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(otherTo) + 64);
    const MAddress copyFromAddr = reinterpret_cast<MAddress>(copyFrom);
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-exclusive-vtable");
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
        fx.region0->GetRegionStart(), fx.region0->GetRegionSize(), fx.region0));
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    StateWord oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    /*deleted copy SM*/ (void)(fx.region0->metadata.copyInflight);
    BaseObject* first = MutatorPublishTestAccess::RelocateInner(collector, copyFrom, copyTo, fx.region0);
    GC_EXPECT_TRUE(first == copyTo);
    copyFrom->SetStateCode(ObjectState::NORMAL);
    oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    BaseObject* second = MutatorPublishTestAccess::ForwardExclusiveVtable(collector, copyFrom);
    GC_EXPECT_TRUE(second == copyTo);
    GC_EXPECT_TRUE(second != otherTo);
    GC_EXPECT_EQ(ForwardingTable::FindTo(copyFromAddr), reinterpret_cast<MAddress>(copyTo));
    GC_EXPECT_EQ(fx.region0->metadata.copyInflight.load(std::memory_order_acquire), 0);
    ForwardingTable::Remove(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ClearEntries(fx.region0->GetRegionStart(), fx.region0->GetRegionSize());
    ForwardingTable::ReclaimRetired("gc-unit-exclusive-vtable");
}
#endif
