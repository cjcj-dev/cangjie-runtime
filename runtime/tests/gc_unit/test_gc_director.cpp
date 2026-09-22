#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Base/TimeUtils.h"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"

#include <chrono>
#include <cmath>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GcDirector, CycleUsesWorkerAccountingAndControlledClock)
{
    ZStatCycle cycle;
    ZStatWorkers workers;
    cycle.Initialize(0);
    const uint64_t start = TimeUtil::NanoSeconds();
    cycle.AtStart(start);
    workers.at_start(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    workers.at_end();
    const auto recorded = workers.stats();
    GC_EXPECT_TRUE(recorded._accumulated_duration > 0.0);
    GC_EXPECT_TRUE(std::fabs(recorded._accumulated_time - 2.0 * recorded._accumulated_duration) < 0.000001);
    const uint64_t end = TimeUtil::NanoSeconds();
    cycle.AtEnd(end, &workers, true, true);
    const auto first = cycle.Stats(end + 1000000000);
    const double wall = static_cast<double>(end - start) / SECOND_TO_NANO_SECOND;
    GC_EXPECT_TRUE(std::fabs(first.serialTime - (wall - recorded._accumulated_duration)) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(first.parallelTime - recorded._accumulated_time) < 0.000001);
    GC_EXPECT_TRUE(std::fabs(first.lastActiveWorkers - 2.0) < 0.000001);
    GC_EXPECT_EQ(first.timeSinceLast, 1.0);
    GC_EXPECT_EQ(first.warmupCycles, 1u);
    const auto reset = workers.stats();
    GC_EXPECT_EQ(reset._accumulated_duration, 0.0);
    GC_EXPECT_EQ(reset._accumulated_time, 0.0);
    workers.at_start(3);
    workers.at_end();
    cycle.AtStart(end);
    cycle.AtEnd(end + 1, &workers, false, false);
    GC_EXPECT_EQ(workers.stats()._accumulated_duration, 0.0);
    const auto unrecorded = cycle.Stats(end + 2);
    GC_EXPECT_TRUE(std::fabs(unrecorded.parallelTime - recorded._accumulated_time) < 0.000001);
    GC_EXPECT_EQ(unrecorded.warmupCycles, 1u);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(GcDirector, WorkerStatsIncludeInFlightBatch)
{
    uint64_t now = 1000000000;
    struct ClockScope {
        const uint64_t* previous;
        explicit ClockScope(const uint64_t& clock) : previous(ZStatWorkers::set_clock_for_test(&clock)) {}
        ~ClockScope() { ZStatWorkers::set_clock_for_test(previous); }
    } clockScope(now);
    ZStatWorkers workers;
    GC_EXPECT_EQ(workers.stats()._accumulated_time, 0.0);
    GC_EXPECT_EQ(workers.stats()._accumulated_duration, 0.0);
    workers.at_start(4);
    const auto stationary = workers.stats();
    GC_EXPECT_EQ(stationary._accumulated_time, 0.0);
    GC_EXPECT_EQ(stationary._accumulated_duration, 0.0);
    // Binary-exact quarter seconds avoid rounding in the equality oracle.
    for (uint64_t step = 1; step <= 4; ++step) {
        now += 250000000;
        const auto inFlight = workers.stats();
        GC_EXPECT_EQ(inFlight._accumulated_time, static_cast<double>(step));
        GC_EXPECT_EQ(inFlight._accumulated_duration, static_cast<double>(step) / 4.0);
        GC_EXPECT_EQ(inFlight._accumulated_time, 4.0 * inFlight._accumulated_duration);
    }
    now += 250000000;
    workers.at_end();
    now += 1000000000;
    const auto done = workers.stats();
    GC_EXPECT_EQ(done._accumulated_time, 5.0);
    GC_EXPECT_EQ(done._accumulated_duration, 1.25);
    GC_EXPECT_EQ(done._accumulated_time, 4.0 * done._accumulated_duration);
    GC_EXPECT_EQ(workers.get_and_reset_duration(), done._accumulated_duration);
    GC_EXPECT_EQ(workers.get_and_reset_time(), done._accumulated_time);
    GC_EXPECT_EQ(workers.stats()._accumulated_duration, 0.0);
    GC_EXPECT_EQ(workers.stats()._accumulated_time, 0.0);
}
#endif

GC_TEST(GcDirector, WarmupCountsOnlyWarmupRequests)
{
    ZStatCycle cycle;
    ZStatWorkers workers;
    cycle.Initialize(0);
    cycle.AtStart(1);
    cycle.AtEnd(2, &workers, false, true);
    GC_EXPECT_EQ(cycle.Stats(3).warmupCycles, 0u);
    GC_EXPECT_FALSE(cycle.Stats(3).isWarm);
    GC_EXPECT_FALSE(cycle.Stats(3).isTimeTrustable);
    for (uint64_t i = 0; i < 4; ++i) {
        cycle.AtStart(10 + 2 * i);
        cycle.AtEnd(11 + 2 * i, &workers, true, true);
        const auto stats = cycle.Stats(12 + 2 * i);
        GC_EXPECT_EQ(stats.isWarm, i >= 2);
        GC_EXPECT_TRUE(stats.isTimeTrustable);
    }
    GC_EXPECT_EQ(cycle.Stats(20).warmupCycles, 3u);
}

// Real allocation -> director thread -> driver -> cycle statistics. No direct
// rule calls or synthesized statistics are supplied to the director.
GC_RUNTIME_OTHER_VM_TEST(GcDirector, ProductWarmupStopsAfterThreeCycles)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    const size_t size = heap.GetMaxCapacity() / 2;
    type->SetInstanceSize(size);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    {
        ScopedObjectAccess access;
        heap.RegisterExportRoot(MObject::NewPinnedObject(type, size));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (heap.old().CycleStats().Stats(TimeUtil::NanoSeconds()).warmupCycles < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = heap.old().CycleStats().Stats(TimeUtil::NanoSeconds());
    const auto before = heap.old().Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto after = heap.old().Snapshot();
    std::fprintf(stderr, "DIRECTOR_WARMUP_TARGET cycles=%u warm=%d trustable=%d before=%llu after=%llu\n",
        stats.warmupCycles, stats.isWarm, stats.isTimeTrustable,
        static_cast<unsigned long long>(before.sequence), static_cast<unsigned long long>(after.sequence));
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_EQ(stats.warmupCycles, 3u);
    GC_EXPECT_EQ(after.sequence, before.sequence);
    GC_EXPECT_TRUE(stats.isWarm);
    GC_EXPECT_TRUE(stats.isTimeTrustable);
}

// Outstanding-queue state is covered through real drivers by
// AllocationStall.ProductLateWaiterRequiresNextCollection.

GC_TEST(GcDirector, CollectionCountsFollowYoungMarkStarts)
{
    // zGeneration.cpp:600,637: the total lives on the heap; a major start
    // snapshots it on ZGenerationOld (zGeneration.cpp:1248,1526).
    const uint32_t prior = Heap::GetHeap().total_collections();
    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(Heap::GetHeap().total_collections(), prior + 1);
    ZStatCycle young;
    ZStatCycle old;
    young.Initialize(0);
    old.Initialize(0);

    ZStatWorkers youngWorkers;
    ZStatWorkers oldWorkers;
    young.AtStart(1);
    young.AtEnd(2, &youngWorkers, true, true);
    old.AtStart(2);
    old.AtEnd(3, &oldWorkers, true, true);
    // Cycle/worker accounting does not move the collection count.
    GC_EXPECT_EQ(Heap::GetHeap().total_collections(), prior + 1);

    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(Heap::GetHeap().total_collections(), prior + 2);
    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(Heap::GetHeap().total_collections(), prior + 3);
    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(Heap::GetHeap().total_collections(), prior + 4);
}

GC_TEST(GenerationState, IndependentPhaseSequenceAndWorkers)
{
    class Probe : public ZGeneration {
    public:
        using ZGeneration::ZGeneration;
        bool should_record_stats() override { return false; }
    };
    Probe young(ZGenerationId::young);
    Probe old(ZGenerationId::old);
    young.InitializeWorkers(2);
    old.InitializeWorkers(2);
    young.SelectReason(GC_REASON_YOUNG);
    young.Begin(1);
    young.PublishPhase(ZGenerationPhase::Mark);
    young.Workers()->set_active_workers(1);
    const auto before = young.Snapshot();

    old.SelectReason(GC_REASON_USER);
    old.Begin(2);
    old.PublishPhase(ZGenerationPhase::Relocate);
    old.Workers()->set_active_workers(2);

    const auto after = young.Snapshot();
    GC_EXPECT_EQ(after.sequence, before.sequence);
    GC_EXPECT_TRUE(after.phase == ZGenerationPhase::Mark);
    GC_EXPECT_EQ(after.reason, GC_REASON_YOUNG);
    GC_EXPECT_TRUE(after.active);
    GC_EXPECT_EQ(young.Workers()->active_workers(), 1u);
    GC_EXPECT_EQ(old.Workers()->active_workers(), 2u);
    GC_EXPECT_TRUE(young.StatHeap() != old.StatHeap());
    GC_EXPECT_TRUE(&young.CycleStats() != &old.CycleStats());

    old.End();
    GC_EXPECT_TRUE(young.Snapshot().active);
    young.End();
}

GC_TEST(GenerationState, FullPrecleanPromotesAllAndRootsComputeThreshold)
{
    class Probe : public ZGenerationYoung {
    public:
        Probe() : ZGenerationYoung(&Heap::page_table(), &Heap::GetHeap().old().forwarding_table(),
                                  &Heap::GetHeap().page_allocator()) {}
        bool should_record_stats() override { return false; }
    };
    Probe young;
    TenuringInputs inputs;
    inputs.softMaxCapacity = 64 * 1024 * 1024;
    inputs.youngAllocated = 4096;
    inputs.youngGarbage = 1024;
    inputs.liveByAge[1] = 1024;
    {
        YoungTypeSetter type(young, ZYoungType::major_full_preclean);
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_EQ(young.tenuring_threshold(), 0u);
        GC_EXPECT_FALSE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
    {
        YoungTypeSetter type(young, ZYoungType::major_full_roots);
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_TRUE(young.tenuring_threshold() > 0u);
        GC_EXPECT_TRUE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
}
