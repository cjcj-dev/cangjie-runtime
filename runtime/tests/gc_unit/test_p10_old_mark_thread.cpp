#include "gc_heap_fixture.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/MutatorManager.h"
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_OTHER_VM_TEST(P10OldMarkThread, ParkedMutatorStackRootConsumedByWorker)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    if (resources.collectorProxy.currentCollector != nullptr) {
        GcHeapFixture::AdoptGenerationIdentity(collector, *resources.collectorProxy.currentCollector);
    }
    resources.collectorProxy.currentCollector = &collector;
    resources.concurrentGcThreadCount = 1;
    for (auto gen : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
        collector.GetGenerationCycle(gen).InitializeWorkers(1);
        collector.GetGenerationCycle(gen).Begin(1);
    }
    ZGlobalsPointers::initialize();
    fx.region0->reset(PageAge::old);
    BaseObject* held = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(held) + held->GetSize());
    GC_EXPECT_FALSE(fx.region0->is_object_strongly_live(from_object(held)));

    Mutator* parked = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(parked != nullptr);
    parked->SetManagedContext(false);
    (void)parked->EnterSaferegion(false);
    ObjectRef* root = parked->AddNativeFrameRoot(held);
    GC_EXPECT_TRUE(root != nullptr);
    GC_EXPECT_TRUE(parked->InSaferegion());

    bool workerSawParked = false;
    collector.testOldMarkThreadResult = [&](Mutator& mutator) {
        if (&mutator == parked) {
            workerSawParked = true;
        }
    };
    collector.SetGCPhase(GCCycleGeneration::OLD, GC_PHASE_ENUM);
    collector.StartOldMarkWork();
    collector.DoOldRoots();
    collector.testOldMarkThreadResult = nullptr;

    const bool live = fx.region0->is_object_strongly_live(from_object(held));
    std::fprintf(stderr, "P10_OLD_MARK_THREAD_ASSERT_EXECUTED worker=%u live=%u epoch=%u\n",
                 unsigned(workerSawParked), unsigned(live), StackWatermark::epoch_id());
    GC_EXPECT_TRUE(workerSawParked);
    GC_EXPECT_TRUE(live);

    parked->RemoveNativeFrameRoot(root);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
