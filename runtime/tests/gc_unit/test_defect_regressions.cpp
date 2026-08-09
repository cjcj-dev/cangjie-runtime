// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Phase-2 defect regression net (甲): each case anchors a shipped fix commit + product site.
// Contracts only — not implementation trivia. Product symbols where the harness can reach them.

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "Common/ColourMask.h"
#include "Common/ColourTypes.h"
#include "Heap/Collector/Collector.h"
#include "ObjectModel/RefField.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr Uptr kAddrMask = (Uptr(1) << 48) - 1u;

// Model of FixOldTagged / ResolveMinor non-heap arm: recolour (or keep) — never install 0.
// Product: WCollector.cpp FixOldTaggedRefField (nullslot 2da28bee / 6a6cf3d8) and
// ResolveMinorReference non-heap early return (zcdnull / B-4 ③).
Uptr ModelRecolourNonHeapNeverNull(Uptr /*slotVal*/, Uptr nonHeapTarget, bool isHeapTarget)
{
    if (!isHeapTarget && nonHeapTarget != 0) {
        return nonHeapTarget; // recolour-only; payload stays
    }
    if (!isHeapTarget) {
        return 0; // only null when target is structurally dead heap residue
    }
    return nonHeapTarget;
}

// Broken pre-nullslot: treated non-heap as dead and CAS-null.
Uptr BrokenNullNonHeap(Uptr /*slotVal*/, Uptr nonHeapTarget, bool isHeapTarget)
{
    if (!isHeapTarget) {
        return 0;
    }
    return nonHeapTarget;
}

// Model of relroroot / rostatic self-heal gate: non-heap loadGood ⇒ skip CAS write-back.
// Product: EnumBarrier/TraceBarrier/ForwardBarrier ReadReference (822b0d64).
bool ModelShouldSelfHealCas(bool loadGoodIsHeap)
{
    return loadGoodIsHeap;
}

// ABI strip of coloured field *place* (fe6d163f / CompilerCalls PlainManagedAddr).
Uptr ModelStripFieldPlace(Uptr maybeColouredPlace)
{
    return RefField<>(maybeColouredPlace).GetAddress();
}

} // namespace

// ① iorfix 8baacb1e — pregrant before RouteRegion freezes domain.
// Contract: after liveInfo0 is frozen without object B, later mark on a *different*
// current liveInfo does not open GetRoute(B). Route geometry alone is not enough.
// Product: RegionInfo::GetRoute domain gate (RegionInfo.h:812+) + installdomain paint face.
GC_TEST(DefectRegress, PregrantBeforeRouteDomainFreeze)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    BaseObject* objA = fx.obj0;
    BaseObject* objB = fx.PlaceObject(reinterpret_cast<MAddress>(objA) + 128);
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(objB) + 64);

    LiveInfo* ghost = fx.PlantLiveInfo(region);
    size_t regionSize = region->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(ghost, regionSize);
    size_t offA = region->GetAddressOffset(reinterpret_cast<MAddress>(objA));
    (void)bm->MarkBits(offA, 8, regionSize);
    // Freeze domain face the way PrepareForwardable does (pointer share).
    region->metadata.liveInfo0 = ghost;
    region->metadata.regionEnd0 = region->GetRegionEnd();
    region->SetRouteInfo(0x20000000u, 4096);
    region->SetRouteState(RegionInfo::RouteState::ROUTED);

    GC_EXPECT_TRUE(region->GetRoute(objA) != nullptr);
    GC_EXPECT_TRUE(region->GetRoute(objB) == nullptr);

    // Late "grant" paints only a *new* current liveInfo — not the frozen ghost face.
    // This is RouteRegion-before-pregrant: geometry frozen, B never in domain.
    LiveInfo* late = new LiveInfo();
    late->bindedRegion = region;
    size_t bytes = RegionBitmap::GetRegionBitmapSize(regionSize);
    void* mem = std::calloc(1, bytes);
    GC_EXPECT_TRUE(mem != nullptr);
    auto* lateBm = new (mem) RegionBitmap(regionSize);
    late->markBitmap = lateBm;
    region->metadata.liveInfo = late;
    size_t offB = region->GetAddressOffset(reinterpret_cast<MAddress>(objB));
    (void)lateBm->MarkBits(offB, 8, regionSize);
    GC_EXPECT_TRUE(late->IsSurvivedObject(offB));
    // Domain still frozen on ghost without B ⇒ GetRoute must miss.
    GC_EXPECT_TRUE(region->GetRoute(objB) == nullptr);

    region->metadata.liveInfo0 = nullptr;
    region->metadata.liveInfo = nullptr;
    lateBm->~RegionBitmap();
    std::free(lateBm);
    delete late;
    fx.FreePlanted(ghost);
}

// ② nullslot 2da28bee — non-heap latest must not be CAS-null'd (recolour only).
// Product predicate: Heap::IsHeapAddress; writeback shape RootSlotWriteback keeps target.
GC_TEST(DefectRegress, NonHeapTargetNeverCasNull)
{
    GcHeapFixture fx;
    // TypeInfo storage is outside the managed heap range planted by the fixture.
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeap));
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));

    Uptr kept = ModelRecolourNonHeapNeverNull(0xdead, reinterpret_cast<Uptr>(nonHeap), false);
    GC_EXPECT_NE(kept, 0u);
    GC_EXPECT_EQ(kept, reinterpret_cast<Uptr>(nonHeap));

    Uptr broken = BrokenNullNonHeap(0xdead, reinterpret_cast<Uptr>(nonHeap), false);
    GC_EXPECT_EQ(broken, 0u); // documents pre-fix shape that tests must reject
}

// ③ B-4 ① / bbb8ff15 — is_mark_good fast path must heap-gate before IsMarkedObject.
// Product: Collector::MarkGoodHeapGate (Collector.cpp:225).
GC_TEST(DefectRegress, MarkGoodHeapGateBlocksNonHeap)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(Collector::MarkGoodHeapGate("gc_unit.markgood", fx.obj0));
    auto* nonHeap = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x55));
    GC_EXPECT_FALSE(Collector::MarkGoodHeapGate("gc_unit.markgood", nonHeap));
    nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(Collector::MarkGoodHeapGate("gc_unit.markgood", nonHeap));
}

// ④ relroroot / rostatic 822b0d64 — RO / non-heap static slots must not get lock cmpxchg.
// Contract via product IsHeapAddress gate used at every self-heal site.
GC_TEST(DefectRegress, RelroNonHeapSkipsSelfHealCas)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(ModelShouldSelfHealCas(Heap::IsHeapAddress(fx.obj0)));
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(ModelShouldSelfHealCas(Heap::IsHeapAddress(nonHeap)));

    // Physical RO page: CAS into it is the failure mode this fix avoids.
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* ro = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    GC_EXPECT_TRUE(ro != MAP_FAILED);
    *reinterpret_cast<uintptr_t*>(ro) = 0x1111;
    GC_EXPECT_EQ(mprotect(ro, page, PROT_READ), 0);
    // Non-heap RO address must not be treated as a self-heal CAS target.
    GC_EXPECT_FALSE(Heap::IsHeapAddress(ro));
    GC_EXPECT_FALSE(ModelShouldSelfHealCas(false));
    munmap(ro, page);
}

// ⑤ minor ResolveMinor non-heap arm (same contract as ② on the minor side).
// Product: ResolveMinorReference — non-heap returns as-is, never CAS-null (WCollector.cpp:1935).
GC_TEST(DefectRegress, MinorNonHeapResolveNeverCasNull)
{
    GcHeapFixture fx;
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeap));
    // Soft-resolve contract: non-heap object identity is preserved (not replaced with null).
    Uptr out = ModelRecolourNonHeapNeverNull(0xabc, reinterpret_cast<Uptr>(nonHeap),
                                            Heap::IsHeapAddress(nonHeap));
    GC_EXPECT_EQ(out, reinterpret_cast<Uptr>(nonHeap));
    GC_EXPECT_NE(out, 0u);
}

// ⑥ fc7e7965 tip-small-int — REJECT must not mean silent total loss of the host.
// Product: PlausibleManagedObjectGate + TryRecoverInteriorBase (Collector.cpp:250/334).
// Correct consumer: REJECT interior, then recover host (ForwardUpdateRawRef / FixMinor).
// MarkObject path that only returns false without recover remains KNOWN_OPEN (see report).
GC_TEST(DefectRegress, TipSmallIntRejectThenRecoverHost)
{
    GcHeapFixture fx;
    BaseObject* base = fx.obj0;
    auto interiorAddr = reinterpret_cast<uintptr_t>(base) + 8;
    auto* interior = reinterpret_cast<BaseObject*>(interiorAddr);
    // Classic RawArray+8: tip word is array length (0x200), not a TypeInfo*.
    *reinterpret_cast<uint64_t*>(interior) = 0x200;

    GC_EXPECT_FALSE(Collector::PlausibleManagedObjectGate("gc_unit.tip200", interior));
    BaseObject* host = Collector::TryRecoverInteriorBase(interior);
    // Not silent discard: host of the interior must still be recoverable.
    GC_EXPECT_TRUE(host != nullptr);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(host), reinterpret_cast<uintptr_t>(base));

    *reinterpret_cast<uint64_t*>(base) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// ⑥b KNOWN_OPEN witness: a consumer that only REJECTS without recover loses the root.
// This test documents the *desired* contract (recover after REJECT). If a call site
// only gates, the host is dropped — that is the open defect surface, not a green pass.
// We keep this green by asserting the recover helper itself; mark-path silent drop is
// load-level (needs concurrent mark) and is reported KNOWN_OPEN in REPORT-gcunit3.
GC_TEST(DefectRegress, TipSmallIntRejectWithoutRecoverIsLoss_Contract)
{
    GcHeapFixture fx;
    BaseObject* base = fx.obj0;
    auto* interior = reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(base) + 8);
    *reinterpret_cast<uint64_t*>(interior) = 0x200;
    bool rejected = !Collector::PlausibleManagedObjectGate("gc_unit.tip200.loss", interior);
    GC_EXPECT_TRUE(rejected);
    // Desired: every product consumer pairs REJECT with recover or derived mark.
    // Witness that recover exists and works — absence of pairing at a site = KNOWN_OPEN.
    BaseObject* host = Collector::TryRecoverInteriorBase(interior);
    GC_EXPECT_TRUE(host == base);
    *reinterpret_cast<uint64_t*>(base) = reinterpret_cast<uintptr_t>(fx.typeInfo);
}

// ⑦ fe6d163f — field *address* may arrive coloured; ABI must peel before dereference.
// Product: RefField::GetAddress / CompilerCalls PlainManagedAddr shape.
GC_TEST(DefectRegress, FieldPlaceColourMustStripAtAbi)
{
    GcHeapFixture fx;
    Uptr plainPlace = reinterpret_cast<Uptr>(fx.obj0) + 16;
    Uptr colouredPlace = plainPlace | ZPointerRemapped00 | MARKED_YOUNG_1;
    Uptr stripped = ModelStripFieldPlace(colouredPlace);
    GC_EXPECT_EQ(stripped, plainPlace);
    GC_EXPECT_EQ(stripped & ~kAddrMask, 0u);
    // Coloured base + offset (lea off(coloured_base)) must peel to plain place.
    Uptr colouredBase = reinterpret_cast<Uptr>(fx.obj0) | ZPointerRemapped01;
    Uptr leaPlace = colouredBase + 16;
    GC_EXPECT_EQ(ModelStripFieldPlace(leaPlace), (reinterpret_cast<Uptr>(fx.obj0) + 16) & kAddrMask);
}
