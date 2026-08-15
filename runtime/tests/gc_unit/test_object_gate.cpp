// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// U5 / U6 — product Collector::TryRecoverInteriorBase + PlausibleManagedObjectGate.
// Product symbols from Collector.cpp (linked via libcangjie-runtime).
// Defect anchors: RawArray+8 / tip-small-int / tip-in-heap (fixinput / nilclass).

#include <cstdint>
#include <cstring>

#include "Heap/Allocator/SlotList.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// U6: product PlausibleManagedObjectGate rejects tip-small-int / null tip.
GC_TEST(ObjectGate, TipSmallIntRejected)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.obj0;
    // Null tip.
    *reinterpret_cast<uint64_t*>(obj) = 0;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    // Small-int tip (RawArray length shape).
    *reinterpret_cast<uint64_t*>(obj) = 42;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    *reinterpret_cast<uint64_t*>(obj) = 8;
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    // Restore for fixture teardown hygiene.
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// U6: product gate rejects tip-in-heap (interior into another managed object).
GC_TEST(ObjectGate, TipInHeapRejected)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.obj0;
    // Tip points into heap (obj1).
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.obj1);
    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit", obj));
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// U6: aligned non-heap tip accepted by product tip branch (region live).
GC_TEST(ObjectGate, PlausibleTipAccepted)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(Collector::PlausibleManagedObjectGate("gc_unit", fx.obj0));
}

// U5: product TryRecoverInteriorBase recovers RawArray+8 host.
GC_TEST(ObjectGate, RawArrayPlus8RecoversBase)
{
    GcHeapFixture fx;
    BaseObject* base = fx.obj0;
    auto interiorAddr = reinterpret_cast<uintptr_t>(base) + 8;
    auto* interior = reinterpret_cast<BaseObject*>(interiorAddr);
    // Length word at interior (tip-small-int); base keeps plausible TypeInfo tip.
    *reinterpret_cast<uint64_t*>(interior) = 16;

    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit.interior", interior));
    BaseObject* host = Collector::TryRecoverInteriorBase(interior);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(host), reinterpret_cast<uintptr_t>(base));
}

// U5: non-interior (plausible tip) does not invent a host.
GC_TEST(ObjectGate, NonInteriorNoFalseRecover)
{
    GcHeapFixture fx;
    BaseObject* host = Collector::TryRecoverInteriorBase(fx.obj0);
    GC_EXPECT_TRUE(host == nullptr);
}

// sizegate2: allocator-header consumers fail closed on a FREE_REGION without
// entering BaseObject::GetSize. This is the deterministic counterpart to the
// stochastic compiler-load GetSize/0x8 signature.
GC_TEST(ObjectGate, HeaderConsumersRejectFreeRegion)
{
    GcHeapFixture fx;
    fx.region0->SetRegionType(RegionInfo::RegionType::FREE_REGION);

    GC_EXPECT_FALSE(MapleRuntime::PlausibleManagedObjectGate("gc_unit.free", fx.obj0));
    GC_EXPECT_TRUE(fx.region0->MarkObject(fx.obj0));

    SlotList slots;
    GC_EXPECT_FALSE(slots.ClearExtraContent(fx.obj0));

    // sizecaller: EnqueueObject is the third header-inline GetSize caller.
    // Gate reject must report already-enqueued so ShouldEnqueue does not SATB-push.
    GC_EXPECT_TRUE(fx.region0->EnqueueObject(fx.obj0, 0));
}
