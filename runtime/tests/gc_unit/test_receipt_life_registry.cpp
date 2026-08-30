// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Product-path checks for destination-life publication.  ZGC makes one
// forwarding-entry CAS the receipt publication edge (zForwarding.inline.hpp:
// 230-251,267-303).  Region reuse adds a prerequisite here: destination life
// registration must finish before that same receipt becomes observable.
// Wiring evidence is the fault-arm patches + seven-cut behavioral tests;
// no static manifest lint — see rev_mw_r7 net-value judgment.

#include "Heap/Allocator/ForwardingTable.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct ProductReceiptFixture {
    GcHeapFixture heap;

    ProductReceiptFixture()
    {
        for (size_t index = 2; index < GcHeapFixture::kUnits; ++index) {
            RegionInfo* region = RegionInfo::InitRegion(
                index, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
            GC_EXPECT_TRUE(region != nullptr);
            region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        }
    }

    BaseObject* Object(size_t regionIndex, size_t offset = 64)
    {
        RegionInfo* region = RegionInfo::GetRegionInfo(regionIndex);
        GC_EXPECT_TRUE(region != nullptr);
        BaseObject* object = heap.PlaceObject(region->GetRegionStart() + offset);
        region->SetRegionAllocPtr(reinterpret_cast<MAddress>(object) + 64);
        return object;
    }

    ForwardingTable::Publication Publication(MAddress lastFrom)
    {
        heap.region0->SetRegionAllocPtr(lastFrom + 64);
        GC_EXPECT_TRUE(ForwardingTable::InstallPublicationBeforeCopy(
            heap.region0->GetRegionStart(), heap.region0->GetRegionSize(), heap.region0));
        ForwardingTable::Publication publication =
            ForwardingTable::EnsurePublicationBeforeCopy(heap.region0,
                                                         reinterpret_cast<MAddress>(heap.obj0));
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        return publication;
    }
};

#if defined(MRT_TESTABLE_INTERNALS)
struct RegistrationRendezvous {
    std::atomic<unsigned> arrived{ 0 };
};

void MeetAtRegistration(void* context)
{
    auto* rendezvous = static_cast<RegistrationRendezvous*>(context);
    rendezvous->arrived.fetch_add(1, std::memory_order_acq_rel);
    while (rendezvous->arrived.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
}
#endif

} // namespace

GC_TEST(ReceiptLifeRegistry, SerialInstallIsPositiveControl)
{
    ProductReceiptFixture fx;
    const MAddress fromA = reinterpret_cast<MAddress>(fx.heap.obj0);
    const MAddress fromB = fromA + 64;
    (void)fx.heap.PlaceObject(fromB);
    const MAddress toA = reinterpret_cast<MAddress>(fx.heap.obj1);
    const MAddress toB = reinterpret_cast<MAddress>(fx.Object(2));
    ForwardingTable::Publication publication = fx.Publication(fromB);

    const ZForwarding::Receipt a = ForwardingTable::InstallMapping(publication, fromA, toA);
    const ZForwarding::Receipt b = ForwardingTable::InstallMapping(publication, fromB, toB);
    ZForwarding* table = ForwardingTable::GetEntries(fromA);
    const MAddress resolvedA = ForwardingTable::FindTo(fromA);
    const MAddress resolvedB = ForwardingTable::FindTo(fromB);
    std::fprintf(stderr,
                 "RECEIPT_LIFE_DETAIL arm=serial a_status=%u b_status=%u a_addr=%#zx b_addr=%#zx "
                 "a_resolved=%#zx b_resolved=%#zx\n",
                 static_cast<unsigned>(a.status), static_cast<unsigned>(b.status),
                 static_cast<size_t>(a.address), static_cast<size_t>(b.address),
                 static_cast<size_t>(resolvedA), static_cast<size_t>(resolvedB));

    GC_EXPECT_TRUE(a.installed && a.status == ZForwarding::Receipt::Status::INSTALLED);
    GC_EXPECT_TRUE(b.installed && b.status == ZForwarding::Receipt::Status::INSTALLED);
    GC_EXPECT_TRUE(table != nullptr);
    GC_EXPECT_EQ(table->find(fromA), toA);
    GC_EXPECT_EQ(table->find(fromB), toB);
    GC_EXPECT_EQ(resolvedA, toA);
    GC_EXPECT_EQ(resolvedB, toB);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(ReceiptLifeRegistry, ConcurrentFirstWinnersPublishBothLivesBeforeReceipts)
{
    GC_EXPECT_EQ(setenv("CJRT_LIFECLOCK_ENFORCE", "1", 1), 0);
    ProductReceiptFixture fx;
    const MAddress fromA = reinterpret_cast<MAddress>(fx.heap.obj0);
    const MAddress fromB = fromA + 64;
    (void)fx.heap.PlaceObject(fromB);
    const MAddress toA = reinterpret_cast<MAddress>(fx.heap.obj1);
    const MAddress toB = reinterpret_cast<MAddress>(fx.Object(2));
    ForwardingTable::Publication publication = fx.Publication(fromB);
    ZForwarding::Receipt receiptA{ 0, false, ZForwarding::Receipt::Status::DESTINATION_UNTRACKED };
    ZForwarding::Receipt receiptB{ 0, false, ZForwarding::Receipt::Status::DESTINATION_UNTRACKED };
    RegistrationRendezvous rendezvous;
    ForwardingTable::SetReceiptLifeRegisterHook(&MeetAtRegistration, &rendezvous);

    std::thread copierA([&]() {
        receiptA = ForwardingTable::InstallMapping(publication, fromA, toA);
    });
    std::thread copierB([&]() {
        receiptB = ForwardingTable::InstallMapping(publication, fromB, toB);
    });
    JoinGuard guardA(copierA);
    JoinGuard guardB(copierB);
    copierA.join();
    copierB.join();
    ForwardingTable::SetReceiptLifeRegisterHook(nullptr, nullptr);

    ZForwarding* table = ForwardingTable::GetEntries(fromA);
    const MAddress resolvedBeforeA = ForwardingTable::FindTo(fromA);
    const MAddress resolvedBeforeB = ForwardingTable::FindTo(fromB);
    std::fprintf(stderr,
                 "RECEIPT_LIFE_DETAIL arm=concurrent arrived=%u a_status=%u b_status=%u "
                 "a_addr=%#zx b_addr=%#zx a_resolved=%#zx b_resolved=%#zx\n",
                 rendezvous.arrived.load(std::memory_order_acquire),
                 static_cast<unsigned>(receiptA.status), static_cast<unsigned>(receiptB.status),
                 static_cast<size_t>(receiptA.address), static_cast<size_t>(receiptB.address),
                 static_cast<size_t>(resolvedBeforeA), static_cast<size_t>(resolvedBeforeB));
    GC_EXPECT_EQ(rendezvous.arrived.load(std::memory_order_acquire), 2U);
    GC_EXPECT_TRUE(receiptA.installed && receiptA.status == ZForwarding::Receipt::Status::INSTALLED);
    GC_EXPECT_TRUE(receiptB.installed && receiptB.status == ZForwarding::Receipt::Status::INSTALLED);
    GC_EXPECT_TRUE(table != nullptr);
    GC_EXPECT_EQ(table->find(fromA), toA);
    GC_EXPECT_EQ(table->find(fromB), toB);
    GC_EXPECT_EQ(resolvedBeforeA, toA);
    GC_EXPECT_EQ(resolvedBeforeB, toB);

    const RegionLifeId oldLifeA = fx.heap.region1->GetRegionLifeId();
    fx.heap.region1->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.heap.region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    fx.heap.obj1 = fx.heap.PlaceObject(toA);
    fx.heap.region1->SetRegionAllocPtr(toA + 64);
    const MAddress resolvedAfterA = ForwardingTable::FindTo(fromA);
    const MAddress resolvedAfterB = ForwardingTable::FindTo(fromB);
    std::fprintf(stderr,
                 "RECEIPT_LIFE_DETAIL arm=reuse old_life=%llu new_life=%llu a_resolved=%#zx b_resolved=%#zx\n",
                 static_cast<unsigned long long>(oldLifeA),
                 static_cast<unsigned long long>(fx.heap.region1->GetRegionLifeId()),
                 static_cast<size_t>(resolvedAfterA), static_cast<size_t>(resolvedAfterB));
    GC_EXPECT_NE(fx.heap.region1->GetRegionLifeId(), oldLifeA);
    GC_EXPECT_EQ(resolvedAfterA, static_cast<MAddress>(0));
    GC_EXPECT_EQ(resolvedAfterB, toB);
}
#endif

GC_TEST(ReceiptLifeRegistry, CapacityRefusesReceiptExplicitly)
{
    ProductReceiptFixture fx;
    const MAddress firstFrom = reinterpret_cast<MAddress>(fx.heap.obj0);
    const MAddress lastFrom = firstFrom + ZForwarding::kToLifeCapacity * 64;
    for (uint8_t i = 1; i <= ZForwarding::kToLifeCapacity; ++i) {
        (void)fx.heap.PlaceObject(firstFrom + static_cast<MAddress>(i) * 64);
    }
    ForwardingTable::Publication publication = fx.Publication(lastFrom);

    for (uint8_t i = 0; i < ZForwarding::kToLifeCapacity; ++i) {
        const MAddress from = firstFrom + static_cast<MAddress>(i) * 64;
        const MAddress to = reinterpret_cast<MAddress>(fx.Object(static_cast<size_t>(i) + 1));
        const ZForwarding::Receipt receipt = ForwardingTable::InstallMapping(publication, from, to);
        GC_EXPECT_TRUE(receipt.installed);
        GC_EXPECT_TRUE(receipt.status == ZForwarding::Receipt::Status::INSTALLED);
        GC_EXPECT_EQ(receipt.address, to);
    }

    const MAddress refusedFrom = lastFrom;
    const MAddress refusedTo = reinterpret_cast<MAddress>(fx.Object(ZForwarding::kToLifeCapacity + 1));
    const ZForwarding::Receipt refused =
        ForwardingTable::InstallMapping(publication, refusedFrom, refusedTo);
    const MAddress refusedLookup = ForwardingTable::GetEntries(firstFrom)->find(refusedFrom);
    std::fprintf(stderr,
                 "RECEIPT_LIFE_DETAIL arm=capacity cap=%u refused_status=%u refused_addr=%#zx lookup=%#zx\n",
                 static_cast<unsigned>(ZForwarding::kToLifeCapacity),
                 static_cast<unsigned>(refused.status), static_cast<size_t>(refused.address),
                 static_cast<size_t>(refusedLookup));
    GC_EXPECT_FALSE(refused.installed);
    GC_EXPECT_EQ(refused.address, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(refused.status == ZForwarding::Receipt::Status::LIFE_REGISTRY_FULL);
    GC_EXPECT_EQ(refusedLookup, static_cast<MAddress>(0));
}
