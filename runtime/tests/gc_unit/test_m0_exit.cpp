// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Constructive M0 classification through the exported compiler runtime entry. The field, phase
// dispatch and unresolved exit are all product code; the test only supplies a deterministic
// collector answer so no workload sampling is involved.

#include <cstring>
#include <mutex>
#include <sstream>

#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Common/ColourPredicates.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/M0ExitDiagnostics.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

// Test-only access to the existing private root-fix entry; this changes no product declaration,
// export or inlining decision. The called symbol is still WCollector from libcangjie-runtime.so.
#define private public
#include "Heap/WCollector/WCollector.h"
#undef private

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadRefField(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);

namespace {

class NoAnswerCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(to_zpointer(reinterpret_cast<MAddress>(object) | remap));
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override { return object; }
};

class IdleProductBarrier final : public Barrier {
public:
    IdleProductBarrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet, BarrierPhase::IDLE) {}
};

class InstalledBarrierScope {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }
    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
};

struct ReadEntryFixture {
    ReadEntryFixture() : barrier(collector, rememberedSet), installed(barrier)
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        static std::once_flag globalRememberedSet;
        std::call_once(globalRememberedSet, [this]() {
            Heap::GetHeap().GetRememberedSet().Initialize(
                heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        });
        heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj0) + 128);
        heap.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj1) + 128);
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(to_zpointer(reinterpret_cast<MAddress>(heap.obj0)));
    }

    GcHeapFixture heap;
    NoAnswerCollector collector;
    RememberedSet rememberedSet;
    IdleProductBarrier barrier;
    InstalledBarrierScope installed;
    RefField<>* field = nullptr;
};

struct RootEntryFixture {
    RootEntryFixture() : collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources())
    {
        ForwardingTable::Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE,
                                    RegionInfo::UNIT_SIZE);
        heap.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        heap.region0->SetInGhostRegion(1);
        heap.region0->SetRouteState(RegionInfo::ROUTED);
        StorePlain(root, from_object(heap.obj0));
    }

    GcHeapFixture heap;
    WCollector collector;
    RootSlot root;
};

} // namespace

GC_TEST(M0Exit, ReadRuntimeEntryClassifiesNoCopyAsS0)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0; // deterministic no-copy/cleared from state
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s0, before.s0 + 1);
    GC_EXPECT_EQ(after.s1, before.s1);
    GC_EXPECT_EQ(after.readBarrier, before.readBarrier + 1);
}

GC_TEST(M0Exit, ReadRuntimeEntryClassifiesPublishedCopyAsS1)
{
    ReadEntryFixture fx;
    BaseObject* publishedCopy = fx.heap.PlaceObject(fx.heap.heapStart + 256);
    std::memcpy(publishedCopy, fx.heap.obj0, fx.heap.obj0->GetSize());
    GC_EXPECT_TRUE(publishedCopy->IsValidObject());
    // Copy bytes exist, and FORWARDED is the product's post-copy publication witness. Deliberately
    // omit the active/retired lookup entry to construct the S1 lifetime/query-loss state.
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s1, before.s1 + 1);
    GC_EXPECT_EQ(after.s0, before.s0);
    GC_EXPECT_EQ(after.readBarrier, before.readBarrier + 1);
}

GC_TEST(M0Exit, ReadRuntimeEntryResolvedNormalPathIsSilent)
{
    ReadEntryFixture fx;
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
    GC_EXPECT_EQ(after.total, before.total);
    GC_EXPECT_EQ(after.s0, before.s0);
    GC_EXPECT_EQ(after.s1, before.s1);
}

GC_TEST(M0Exit, RootFixProductExitClassifiesNoCopyAsS0)
{
    RootEntryFixture fx;
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    (void)fx.collector.FixMinorEvacuatedSlot(fx.root, nullptr);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s0, before.s0 + 1);
    GC_EXPECT_EQ(after.rootFix, before.rootFix + 1);
    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(fx.root.LoadPlain())),
                 reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_TEST(M0Exit, RootFixProductExitClassifiesPublishedCopyAsS1)
{
    RootEntryFixture fx;
    std::memcpy(fx.heap.obj1, fx.heap.obj0, fx.heap.obj0->GetSize());
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    (void)fx.collector.FixMinorEvacuatedSlot(fx.root, nullptr);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s1, before.s1 + 1);
    GC_EXPECT_EQ(after.rootFix, before.rootFix + 1);
    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(fx.root.LoadPlain())),
                 reinterpret_cast<uintptr_t>(fx.heap.obj0));
}
