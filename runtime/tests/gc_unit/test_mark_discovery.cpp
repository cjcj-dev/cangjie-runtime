// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "ObjectModel/RefField.inline.h"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
extern "C" int CJ_ScheduleManagerInit();
namespace {
class DiscoveryRuntime final : public Runtime {
public:
    explicit DiscoveryRuntime(MutatorManager& manager) {
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
    }
    ~DiscoveryRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
private:
    Concurrency concurrency;
};
}

namespace {
// Enter the old product mark phase with a weak-only referent. Discovery is
// observed by consuming the product queue, never by calling discover_reference.
void RunMarkDiscovery1036(bool finalizable, bool strong, bool young = false, size_t workers = 1,
                          bool oldReferent = false, bool cycle = false)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    DiscoveryRuntime runtime(manager);
    GcHeapFixture fx;
    fx.region0->reset(young ? PageAge::eden : PageAge::old);
    fx.region1->reset(young && !oldReferent ? PageAge::eden : PageAge::old);
    fx.region0->SetRegionRole(ZPageRole::RecentFull);
    fx.region1->SetRegionRole(ZPageRole::RecentFull);
    alignas(TypeInfo) unsigned char targetTypeStorage[sizeof(TypeInfo)];
    std::memcpy(targetTypeStorage, fx.typeInfo, sizeof(TypeInfo));
    auto* targetType = reinterpret_cast<TypeInfo*>(targetTypeStorage);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(targetTypeStorage), sizeof(targetTypeStorage));
    fx.obj1->SetClassInfo(targetType);
    if (!strong) fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& referent = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    referent.StoreColoured(StoreGoodPointer(fx.obj1));
    HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE)
        .StoreColoured(cycle ? StoreGoodPointer(fx.obj0) : zpointer::null);
    auto& heap = Heap::GetHeap();
    heap.young().InitializeWorkers(workers);
    heap.old().InitializeWorkers(workers);
    heap.old().set_phase(ZGenerationPhase::Relocate);
    U64 handle = 0;
    if (finalizable) heap.GetFinalizerProcessor().RegisterFinalizer(fx.obj0);
    else handle = heap.RegisterExportRoot(cycle ? fx.obj1 : fx.obj0);
    if (young) {
        heap.young().set_phase(ZGenerationPhase::MarkComplete);
        YoungTypeSetter type(heap.young(), ZYoungType::minor);
        heap.young().pause_mark_start();
        heap.young().concurrent_mark();
    } else {
        {
            ScopedStopTheWorld pause("reference discovery mark-start", false);
            heap.old().mark_start();
        }
        heap.old().concurrent_mark();
    }
    const bool targetStrong = fx.region1->is_object_strongly_live(from_object(fx.obj1));
    const bool targetLive = fx.region1->is_object_live(from_object(fx.obj1));
    auto& processor = heap.GetFinalizerProcessor().GetReferenceProcessor();
    const size_t discovered = processor.Discovered(ReferenceType::WEAK);
    processor.ProcessReferences([](BaseObject*) { return false; });
    processor.EnqueueReferences([](BaseObject*) { return true; });
    const bool cleared = referent.GetTargetObject() == zaddress::null;
    if (handle != 0) heap.RemoveExportObject(handle);
    if (finalizable) {
        heap.GetFinalizerProcessor().VisitNativePointers([](NativeSlot& root) {
            root.StoreColoured(zpointer::null);
        });
    }
    std::fprintf(stderr,
        "MARK1036_TARGET finalizable=%d strong=%d young=%d workers=%zu old_referent=%d cycle=%d discovered=%zu target_live=%d target_strong=%d cleared=%d\n",
        finalizable, strong, young, workers, oldReferent, cycle, discovered, targetLive, targetStrong, cleared);
    GC_EXPECT_TRUE(oldReferent ? (!targetStrong && !targetLive && !cleared && discovered == 0) :
        (strong || young || cycle) ? (targetStrong && targetLive && !cleared && discovered == 0) :
        finalizable ? (!targetStrong && targetLive && !cleared && discovered == 0) :
                      (!targetStrong && !targetLive && cleared && discovered == 1));
}
}

GC_OTHER_VM_TEST(MarkDiscovery1036, OldWeakReferentDiscoveredAndCleared)
{
    RunMarkDiscovery1036(false, false);
}
GC_OTHER_VM_TEST(MarkDiscovery1036, FinalizableClosureFollowsWithoutDiscovery)
{
    RunMarkDiscovery1036(true, false);
}
GC_OTHER_VM_TEST(MarkDiscovery1036, StrongFieldRemainsStrong)
{
    RunMarkDiscovery1036(false, true);
}

GC_OTHER_VM_TEST(MarkDiscovery1036, YoungWeakReferentRemainsStrong)
{
    RunMarkDiscovery1036(false, false, true);
}
GC_OTHER_VM_TEST(MarkDiscovery1036, YoungWeakReferentMultipleWorkers)
{
    RunMarkDiscovery1036(false, false, true, 2);
}
GC_OTHER_VM_TEST(MarkDiscovery1036, OldWeakReferentMultipleWorkers)
{
    RunMarkDiscovery1036(false, false, false, 2);
}

GC_OTHER_VM_TEST(MarkDiscovery1036, YoungReferenceToOldReferentNotDiscovered)
{
    RunMarkDiscovery1036(false, false, true, 1, true);
}
GC_OTHER_VM_TEST(MarkDiscovery1036, StronglyLiveReferentNotDiscovered)
{
    RunMarkDiscovery1036(false, false, false, 1, false, true);
}
