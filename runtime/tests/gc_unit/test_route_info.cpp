// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U3 — product RouteInfo::GetRoute + RegionInfo domain gate.
// Product symbols: MapleRuntime::RouteInfo::GetRoute, RegionInfo::GetRoute,
// RegionInfo::SetRouteInfo, RegionInfo::BindLiveInfo0FromLiveIfNull.
// Defect anchor: GetRoute nullptr ⇒ ior; installdomain BindLiveInfo0FromLiveIfNull.

#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct ExactRouteFixture {
    GcHeapFixture fx;
    RegionInfo* region;
    BaseObject* first;
    BaseObject* second;
    LiveInfo* live;

    explicit ExactRouteFixture(bool useRealProducer = false)
        : region(fx.region0), first(nullptr), second(nullptr), live(nullptr)
    {
        constexpr size_t kObjectSize = 48;
        fx.typeInfo->SetInstanceSize(kObjectSize - TYPEINFO_PTR_SIZE);
        const MAddress start = region->GetRegionStart();
        for (size_t offset = 0; offset < 2 * kObjectSize; offset += sizeof(uint64_t)) {
            *reinterpret_cast<uint64_t*>(start + offset) = reinterpret_cast<uintptr_t>(fx.typeInfo);
        }
        first = from_region_addr(start);
        second = from_region_addr(start + kObjectSize);
        fx.obj0 = first;
        region->SetRegionAllocPtr(start + 2 * kObjectSize);
        region->SetRegionType(RegionInfo::RegionType::FROM_REGION);

        live = fx.PlantLiveInfo(region);
        RegionBitmap* bitmap = fx.PlantMarkBitmap(live, region->GetRegionSize());
        (void)bitmap->MarkBits(0, kObjectSize, region->GetRegionSize());
        (void)bitmap->MarkBits(kObjectSize, kObjectSize, region->GetRegionSize());
        region->BindLiveInfo0FromLiveIfNull();
        if (useRealProducer) {
            // Exercise the production publication walk: it freezes the exact
            // starts while headers are readable and publishes the ghost carrier.
            region->AddLiveByteCount(2 * kObjectSize);
            region->PrepareForwardableRegion(region->GetMarkView<Generation::Old>());
        } else {
            // Legacy geometry cells intentionally retain their hand-fed table;
            // they are not the product接线证明 for exact-start.
            region->RecordRouteStart(0);
            region->RecordRouteStart(kObjectSize);
        }
    }

    ~ExactRouteFixture()
    {
        if (region->IsGhostFromRegion()) {
            region->DispelGhostFromRegion();
        }
        region->SetRouteState(RegionInfo::NORMAL);
        region->FreeCompactRouteTable();
        region->FreeRouteStartTable();
        region->metadata.liveInfo = nullptr;
        fx.FreePlanted(live);
    }

    void ExpectOnlyExactStartsAdmitted()
    {
        const MAddress start = region->GetRegionStart();
        unsigned admittedMask = 0;
        for (size_t offset = 0; offset <= 40; offset += 8) {
            OptionalRouteTicket ticket = region->AdmitForRoute(from_region_addr(start + offset));
            admittedMask |= ticket.has_value() ? (1U << (offset / 8)) : 0U;
            GC_EXPECT_EQ(ticket.has_value(), offset == 0);
        }
        const bool secondAdmitted = region->AdmitForRoute(second).has_value();
        GC_EXPECT_TRUE(secondAdmitted);
        std::fprintf(stderr,
                     "DETAIL exact_start state=%u object_size=48 offsets=0,8,16,24,32,40 admitted_mask=%#x second_start=48 second_admitted=%u\n",
                     static_cast<unsigned>(region->GetRouteState()), admittedMask,
                     static_cast<unsigned>(secondAdmitted));
    }

    ForwardingTable::Publication InstallReceipts()
    {
        const MAddress start = region->GetRegionStart();
        ForwardingTable::ClearEntries(start, region->GetRegionSize());
        ForwardingTable::ReclaimRetired("exact-route-fixture-reset");
        GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(start, region->GetRegionSize()));
        GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(start, region->GetRegionSize(), region));
        ForwardingTable::Publication publication =
            ForwardingTable::EnsurePublicationBeforeCopy(region, start);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        return publication;
    }

    void ExpectOnlyExactStartsThroughProductEntry(WCollector& collector)
    {
#if defined(MRT_GC_UNIT_TESTS)
        const MAddress start = region->GetRegionStart();
        unsigned admittedMask = 0;
        for (size_t offset = 0; offset <= 40; offset += 8) {
            auto result = collector.PlanRouteLookupForTest(from_region_addr(start + offset));
            GC_EXPECT_TRUE(result.phaseAllowed);
            GC_EXPECT_TRUE(result.heapAddress);
            GC_EXPECT_TRUE(result.retained);
            admittedMask |= result.plan.dest != nullptr ? (1U << (offset / 8)) : 0U;
            GC_EXPECT_EQ(result.plan.dest != nullptr, offset == 0);
        }
        auto secondResult = collector.PlanRouteLookupForTest(second);
        GC_EXPECT_TRUE(secondResult.phaseAllowed);
        GC_EXPECT_TRUE(secondResult.heapAddress);
        GC_EXPECT_TRUE(secondResult.retained);
        GC_EXPECT_TRUE(secondResult.plan.dest != nullptr);
        std::fprintf(stderr,
                     "DETAIL exact_start_product_entry state=%u phase=%u object_size=48 offsets=0,8,16,24,32,40 admitted_mask=%#x second_start=48 second_admitted=%u\n",
                     static_cast<unsigned>(region->GetRouteState()),
                     static_cast<unsigned>(collector.GetGCPhase()), admittedMask,
                     static_cast<unsigned>(secondResult.plan.dest != nullptr));
#else
        // The default product SO intentionally omits the test bridge; keep the
        // legacy cell buildable there while the ON construct is the接线证明.
        ExpectOnlyExactStartsAdmitted();
#endif
    }
};

std::atomic<bool> gRouteStateReaderPaused { false };
std::atomic<bool> gRouteStateReaderResume { false };

#if defined(MRT_GC_UNIT_TESTS)
void PauseAfterRouteSnapshotLoad(RegionInfo*)
{
    gRouteStateReaderPaused.store(true, std::memory_order_release);
    while (!gRouteStateReaderResume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
#endif

} // namespace

GC_TEST(RouteInfo, ExactStartCapabilityAcrossRouteStates)
{
    {
        ExactRouteFixture route(/*useRealProducer=*/true);
        route.region->SetRouteInfo(route.fx.region1->GetRegionStart(), 96);
        route.region->SetRouteState(RegionInfo::ROUTED);
        WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
#if defined(MRT_GC_UNIT_TESTS)
        collector.SetGCPhase(GCPhase::GC_PHASE_IDLE);
        auto idle = collector.PlanRouteLookupForTest(route.first);
        GC_EXPECT_FALSE(idle.phaseAllowed);
        GC_EXPECT_FALSE(idle.retained);
        GC_EXPECT_TRUE(idle.plan.dest == nullptr);
        const auto nonHeap = collector.PlanRouteLookupForTest(reinterpret_cast<BaseObject*>(0x1234));
        GC_EXPECT_FALSE(nonHeap.heapAddress);
        GC_EXPECT_FALSE(nonHeap.phaseAllowed);
        GC_EXPECT_FALSE(nonHeap.retained);
        collector.SetGCPhase(GCPhase::GC_PHASE_PREFORWARD);
#endif
        route.ExpectOnlyExactStartsThroughProductEntry(collector);
    }
    {
        ExactRouteFixture compact;
        compact.region->EnsureCompactRouteTable();
        compact.region->RecordCompactRoute(0, reinterpret_cast<MAddress>(compact.first));
        compact.region->RecordCompactRoute(48, reinterpret_cast<MAddress>(compact.second));
        compact.region->SetRouteState(RegionInfo::COMPACTED);
        compact.ExpectOnlyExactStartsAdmitted();
    }
    {
        ExactRouteFixture forwarded;
        ForwardingTable::Publication publication = forwarded.InstallReceipts();
        const MAddress first = reinterpret_cast<MAddress>(forwarded.first);
        const MAddress second = reinterpret_cast<MAddress>(forwarded.second);
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, first, first), first);
        GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, second, second), second);
        publication = ForwardingTable::Publication();
        forwarded.region->SetRouteState(RegionInfo::FORWARDED);
        forwarded.ExpectOnlyExactStartsAdmitted();
    }
}

GC_TEST(RouteInfo, CompactExactMissFailsClosedAndIdentityReceiptIsExplicit)
{
    ExactRouteFixture compact;
    const MAddress from = reinterpret_cast<MAddress>(compact.first);
    compact.region->EnsureCompactRouteTable();
    compact.region->RecordCompactRoute(0, from);
    compact.region->SetRouteState(RegionInfo::COMPACTED);

    OptionalRouteTicket ticket = compact.region->AdmitForRoute(compact.first);
    GC_EXPECT_TRUE(ticket.has_value());
    RegionInfo::CompactRouteTable* routes = compact.region->LoadCompactRouteTable();
    GC_EXPECT_TRUE(routes != nullptr);
    GC_EXPECT_EQ(routes->erase(0), static_cast<size_t>(1));
    GC_EXPECT_TRUE(compact.region->GetRoute(ticket.value()) == nullptr);

    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    GC_EXPECT_TRUE(collector.FindToVersion(compact.first).state() ==
                   FindToVersionResult::State::NotForwarded);

    ForwardingTable::Publication publication = compact.InstallReceipts();
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(publication, from, from), from);
    publication = ForwardingTable::Publication();
    const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(from);
    GC_EXPECT_EQ(lookup.to, from);
    GC_EXPECT_TRUE(lookup.answer == ForwardingTable::ToAnswer::ArmedHit);
    GC_EXPECT_TRUE(compact.region->GetRoute(ticket.value()) == compact.first);
    BaseObject* productIdentity = collector.FindToVersion(compact.first).found();
    GC_EXPECT_TRUE(productIdentity == compact.first);
    std::fprintf(stderr,
                 "DETAIL compact_exact_miss erased=1 miss_route=null identity_receipt=%p from=%p product_find=%p\n",
                 compact.first, compact.first, productIdentity);
}

GC_TEST(RouteInfo, RouteStateAndLifeAreOneReaderSnapshot)
{
#if defined(MRT_GC_UNIT_TESTS)
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRouteState(RegionInfo::COMPACTED);
    gRouteStateReaderPaused.store(false, std::memory_order_relaxed);
    gRouteStateReaderResume.store(false, std::memory_order_relaxed);
    RegionInfo::SetRouteStateReadTestHook(PauseAfterRouteSnapshotLoad);

    std::atomic<RegionInfo::RouteState> observed { RegionInfo::COMPACTED };
    std::thread reader([&]() { observed.store(region->GetRouteState(), std::memory_order_release); });
    while (!gRouteStateReaderPaused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    region->BumpRegionLifeId();
    region->SetRouteState(RegionInfo::NORMAL);
    gRouteStateReaderResume.store(true, std::memory_order_release);
    reader.join();
    const size_t hookCalls = RegionInfo::RouteStateReadTestHookCalls();
    RegionInfo::SetRouteStateReadTestHook(nullptr);

    GC_EXPECT_EQ(hookCalls, static_cast<size_t>(1));
    const RegionInfo::RouteState resumed = observed.load(std::memory_order_acquire);
    GC_EXPECT_TRUE(resumed == RegionInfo::NORMAL);
    GC_EXPECT_TRUE(region->GetRouteState() == RegionInfo::NORMAL);
    std::fprintf(stderr,
                 "DETAIL route_snapshot hook_calls=1 writer_state=%u reader_resumed=%u life=%llu stamp=%llu\n",
                 static_cast<unsigned>(region->GetRouteState()), static_cast<unsigned>(resumed),
                 static_cast<unsigned long long>(region->GetRegionLifeId()),
                 static_cast<unsigned long long>(region->GetRouteStateLifeId()));
#endif
}

GC_TEST(RouteInfo, FailedRouteStateCasDoesNotStampOldStateIntoNewLife)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRouteState(RegionInfo::FORWARDED);
    region->BumpRegionLifeId();
    GC_EXPECT_FALSE(region->CompareExchangeRouteState(RegionInfo::ROUTED, RegionInfo::NORMAL));
    const RegionInfo::RouteState observed = region->GetRouteState();
    const RegionLifeId stamp = region->GetRouteStateLifeId();
    const RegionLifeId life = region->GetRegionLifeId();
    GC_EXPECT_TRUE(observed == RegionInfo::NORMAL);
    GC_EXPECT_NE(stamp, life);

    region->SetRouteState(RegionInfo::ROUTED);
    GC_EXPECT_TRUE(region->CompareExchangeRouteState(RegionInfo::ROUTED, RegionInfo::FORWARDED));
    GC_EXPECT_TRUE(region->GetRouteState() == RegionInfo::FORWARDED);
    GC_EXPECT_EQ(region->GetRouteStateLifeId(), region->GetRegionLifeId());
    std::fprintf(stderr,
                 "DETAIL route_cas_fail cas_success=0 observed=%u life=%llu stale_stamp=%llu same_life_cas=1 forwarded=%u\n",
                 static_cast<unsigned>(observed), static_cast<unsigned long long>(life),
                 static_cast<unsigned long long>(stamp),
                 static_cast<unsigned>(region->GetRouteState()));
}

// U3: product RouteInfo::GetRoute region1 geometry (LiveInfo.cpp:15-18).
GC_TEST(RouteInfo, InsertLookupIdempotentRegion1)
{
    RouteInfo ri;
    constexpr uintptr_t kToStart = 0x20000000u;
    constexpr uint32_t kUsed = 4096;
    ri.SetRouteInfo(kToStart, kUsed);

    GC_EXPECT_EQ(ri.GetRoute(0), kToStart);
    GC_EXPECT_EQ(ri.GetRoute(64), kToStart + 64);
    GC_EXPECT_EQ(ri.GetRoute(4095), kToStart + 4095);
    GC_EXPECT_EQ(ri.GetRoute(64), kToStart + 64);
}

// U3: product RegionInfo::GetRoute domain gate — null ghost / unmarked ⇒ nullptr.
GC_TEST(RouteInfo, MissingDomainReturnsNullNotGarbage)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* obj = fx.obj0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    region->SetRouteInfo(0x20000000u, 4096);
    // ROUTED up front, not just before the positive arm: RegionInfo::GetRoute(RouteTicket) refuses
    // any region still in NORMAL (RegionInfo.h:1806-1809), so with the state left at NORMAL the
    // three rejections below would read green even if the domain gate admitted everything.  The
    // gate under test is AdmitForRoute; give it the only state in which it can be observed.
    region->SetRouteState(RegionInfo::ROUTED);

    // No ghost liveInfo0 → reject even if route geometry is set.
    GC_EXPECT_TRUE(region->GetRouteForProbe(obj) == nullptr);

    LiveInfo* live = fx.PlantLiveInfo(region);
    size_t regionSize = region->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    // Marked but ghost still null → still reject.
    (void)bm->MarkBits(offset, 8, regionSize);
    GC_EXPECT_TRUE(region->GetRouteForProbe(obj) == nullptr);

    // Bind ghost; still unmarked offset stays null — re-use unmarked sibling.
    BaseObject* sibling = fx.PlaceObject(reinterpret_cast<MAddress>(obj) + 128);
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_TRUE(region->GetRouteForProbe(sibling) == nullptr);
    region->RecordRouteStart(offset);

    // Positive control, and it needs the route to actually exist.  Geometry plus a survivor bit is
    // not a forwarding: RegionInfo::GetRoute(RouteTicket) (RegionInfo.h:1806-1809) refuses unless
    // the region has reached ROUTED or FORWARDED, because FreeCompactRouteTable publishes NORMAL
    // before detaching a compact table and a reader that loses that race must soft-miss rather than
    // read compact destinations as prefix-sum geometry.
    //
    // OpenJDK draws the same line structurally rather than by state check: ZForwarding::find returns
    // `entry.populated() ? ... : zaddress::null` (zForwarding.inline.hpp:248-252), and a forwarding
    // exists at all only for pages placed in the relocation set.  A region still in NORMAL has no
    // forwarding to consult, so null is the right answer in both designs.
    //
    // This test previously stopped at the geometry and expected a destination anyway, which made it
    // the only red test in the suite while the product was correct.  Keeping the arm rather than
    // deleting it: without it the three rejections above would also pass if the gate simply always
    // returned null.
    BaseObject* to = region->GetRouteForProbe(obj);
    GC_EXPECT_TRUE(to != nullptr);
    uintptr_t pre = region->GetPreLiveBytesInGhostRegionForProbe(reinterpret_cast<MAddress>(obj));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(to), 0x20000000u + pre);

    region->SetRouteState(RegionInfo::NORMAL);
    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// U3: product SetRouteInfo overwrites geometry.
GC_TEST(RouteInfo, SetRouteInfoOverwrites)
{
    RouteInfo ri;
    ri.SetRouteInfo(0x1000u, 100);
    GC_EXPECT_EQ(ri.GetRoute(10), 0x1000u + 10u);
    ri.SetRouteInfo(0x9000u, 100);
    GC_EXPECT_EQ(ri.GetRoute(10), 0x9000u + 10u);
}

// Eth: to-region1 capacity boundary — preLiveBytes at to1used-1 stays in region1.
GC_TEST(RouteInfo, ToRegion1BoundaryInclusive)
{
    RouteInfo ri;
    constexpr uintptr_t kToStart = 0x30000000u;
    constexpr uint32_t kUsed = 256;
    ri.SetRouteInfo(kToStart, kUsed);
    GC_EXPECT_EQ(ri.GetRoute(0), kToStart);
    GC_EXPECT_EQ(ri.GetRoute(kUsed - 1), kToStart + (kUsed - 1));
}

// Eth: overflow into to-region2 via unit index (fixture region1 as to2).
// Product LiveInfo.cpp:15-24 — preLiveBytes >= to1used ⇒ GetRegionInfo(to2).
GC_TEST(RouteInfo, ToRegion2WhenPreLiveExceedsTo1)
{
    GcHeapFixture fx;
    RouteInfo ri;
    constexpr uintptr_t kTo1 = 0x30000000u;
    constexpr uint32_t kUsed = 64;
    // Fixture units 0 and 1; use unit index 1 as to-region2.
    ri.SetRouteInfo(kTo1, kUsed, /*to2=*/1u);
    uintptr_t got = ri.GetRoute(kUsed); // first byte in region2
    uintptr_t expect = fx.region1->GetRegionStart() + 0;
    GC_EXPECT_EQ(got, expect);
    got = ri.GetRoute(kUsed + 32);
    expect = fx.region1->GetRegionStart() + 32;
    GC_EXPECT_EQ(got, expect);
}

// U4 surface: product BindLiveInfo0FromLiveIfNull on RegionInfo.
GC_TEST(RouteInfo, BindLiveInfo0FromLiveIfNull)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    region->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    LiveInfo* liveA = fx.PlantLiveInfo(region);
    LiveInfo* liveB = new LiveInfo();
    liveB->bindedRegion = region;

    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    region->metadata.liveInfo = liveB;
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    region->metadata.liveInfo = nullptr;
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_TRUE(region->GetLiveInfo0ForProbe() == nullptr);

    region->metadata.liveInfo = liveA;
    GC_EXPECT_TRUE(ForwardingTable::PreparePublicationGeneration(
        region->GetRegionStart(), region->GetRegionSize()));
    GC_EXPECT_TRUE(ForwardingTable::InsertProvisional(
        region->GetRegionStart(), region->GetRegionSize(), region));
    region->BindLiveInfo0FromLiveIfNull();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(region->GetLiveInfo0ForProbe()),
                 reinterpret_cast<uintptr_t>(liveA));

    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
    region->metadata.liveInfo = nullptr;
    fx.FreePlanted(liveA);
    delete liveB;
}
