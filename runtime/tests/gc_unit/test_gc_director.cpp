#include "gc_generation_test.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
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
#include "Inspector/ProfilerAgentImpl.h"
#include "gc_unittest.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <thread>
#include <limits>

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
    const auto before = heap.old().seqnum();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto after = heap.old().seqnum();
    std::fprintf(stderr, "DIRECTOR_WARMUP_TARGET cycles=%u warm=%d trustable=%d before=%llu after=%llu\n",
        stats.warmupCycles, stats.isWarm, stats.isTimeTrustable,
        static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_EQ(stats.warmupCycles, 3u);
    GC_EXPECT_EQ(after, before);
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
    young.set_phase(ZGenerationPhase::Mark);
    young.Workers()->set_active_workers(1);
    const auto before = young.seqnum();

    old.set_phase(ZGenerationPhase::Relocate);
    old.Workers()->set_active_workers(2);

    const auto after = young.seqnum();
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_TRUE(young.is_phase_mark());
    GC_EXPECT_EQ(young.Workers()->active_workers(), 1u);
    GC_EXPECT_EQ(old.Workers()->active_workers(), 2u);
    GC_EXPECT_TRUE(young.StatHeap() != old.StatHeap());
    GC_EXPECT_TRUE(&young.CycleStats() != &old.CycleStats());

}

GC_TEST(GenerationState, FullPrecleanPromotesAllAndRootsComputeThreshold)
{
    class Probe : public ZGenerationYoung {
    public:
        Probe() : ZGenerationYoung(&Heap::page_table(), &Heap::GetHeap().old().forwarding_table(),
                                  &Heap::GetHeap().page_allocator()) {}
        bool should_record_stats() override { return false; }
    };
    GenerationFixtureState::Scope generationState;
    Probe young;
    TenuringInputs inputs;
    inputs.softMaxCapacity = 64 * 1024 * 1024;
    inputs.youngAllocated = 4096;
    inputs.youngGarbage = 1024;
    inputs.liveByAge[1] = 1024;
    {
        YoungTypeSetter type(young, ZYoungType::major_full_preclean);
        inputs.promoteAll = true;
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_EQ(young.tenuring_threshold(), 0u);
        GC_EXPECT_FALSE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
    {
        YoungTypeSetter type(young, ZYoungType::major_full_roots);
        inputs.promoteAll = false;
        young.SelectTenuringThreshold(inputs);
        GC_EXPECT_TRUE(young.tenuring_threshold() > 0u);
        GC_EXPECT_TRUE(young.IsMajorRoots());
    }
    GC_EXPECT_TRUE(young.YoungType() == ZYoungType::none);
}


// #906: InitCJRuntime produces the effective flag values in the product SO.
// Explicitness is independent from the value: zero and -1 are real inputs.
#include "Heap/z/zHeuristics.hpp"
#include "CjScheduler.h"
#include <sys/wait.h>
#include <unistd.h>
#include "Heap/z/zPage.hpp"
namespace {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
struct TenuringCollectionResult {
    uint32_t threshold = 0;
    PageAge survivorAge = PageAge::eden;
    bool full = false;
};
void* CollectWithTenuringFlags(void* context)
{
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    const U64 root = Heap::GetHeap().RegisterExportRoot(MCC_NewObject(type, 4096));
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    auto& result = *static_cast<TenuringCollectionResult*>(context);
    if (result.full) { Heap::GetHeap().RequestGC(GC_REASON_USER); }
    result.threshold = Heap::GetHeap().young().tenuring_threshold();
    auto* survivor = Heap::GetHeap().GetExportObject(root);
    result.survivorAge = Heap::page(reinterpret_cast<uintptr_t>(survivor))->age();
    Heap::GetHeap().RemoveExportObject(root);
    Mutator::GetMutator()->SetManagedContext(true);
    return nullptr;
}

void CheckTenuringResult(const TenuringCollectionResult& result, uint32_t selected)
{
    std::fprintf(stderr, "TENURING_CONSUMER_TARGET actual=%u expected=%u\n", result.threshold, selected);
    const bool thresholdMatches = result.threshold == selected;
    const PageAge expectedAge = result.full || selected == 0 ? PageAge::old : PageAge::survivor1;
    std::fprintf(stderr, "TENURING_PROMOTION_TARGET actual=%u expected=%u\n",
                 untype(result.survivorAge), untype(expectedAge));
    const bool promotionMatches = result.survivorAge == expectedAge;
    // Evaluate both product results before the single invariant assertion so
    // a threshold failure cannot hide the actual promotion observation.
    std::fprintf(stderr, "TENURING_RESULT_ASSERT threshold_match=%d promotion_match=%d\n",
                 thresholdMatches, promotionMatches);
    GC_EXPECT_TRUE(thresholdMatches && promotionMatches);
}
uint32_t environmentSelected;
int32_t RunEnvironmentTenuringMain()
{
    try {
        TenuringCollectionResult result;
        CollectWithTenuringFlags(&result);
        CheckTenuringResult(result, environmentSelected);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "TENURING_ENV_ASSERT_FAILED %s\n", error.what());
        return 1;
    }
}

void CheckTenuringFlags(size_t heapKB, uint32_t workers, bool maxSet, uint32_t maximum,
                        bool overrideSet, int32_t overrideValue, bool environment = false, bool full = false)
{
    // The managed executable entry exits with its main result. Isolate that
    // normal product lifecycle in a child and assert its actual exit status.
    if (environment) {
        const pid_t child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child != 0) {
            int status = 0;
            GC_EXPECT_EQ(waitpid(child, &status, 0), child);
            std::fprintf(stderr, "TENURING_MANAGED_EXIT status=%d\n", status);
            GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
    }
    RuntimeParam params{};
    params.heapParam.heapSize = heapKB;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = workers;
    params.gcParam.maxTenuringThresholdSet = maxSet;
    params.gcParam.maxTenuringThreshold = maximum;
    params.gcParam.zTenuringThresholdSet = overrideSet;
    params.gcParam.zTenuringThreshold = overrideValue;
    if (environment) {
        setenv("cjHeapSize", (std::to_string(heapKB) + "KB").c_str(), 1);
        setenv("cjProcessorNum", "1", 1);
        setenv("cjConcGCThreads", std::to_string(workers).c_str(), 1);
        if (maxSet) {
            setenv("cjMaxTenuringThreshold", std::to_string(maximum).c_str(), 1);
        } else {
            unsetenv("cjMaxTenuringThreshold");
        }
        if (overrideSet) {
            setenv("cjZTenuringThreshold", std::to_string(overrideValue).c_str(), 1);
        } else {
            unsetenv("cjZTenuringThreshold");
        }
        MRT_CjRuntimeInit();
    } else {
        GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    }
    const size_t overhead = ZHeuristics::relocation_headroom();
    const size_t budget = ZHeuristics::significant_young_overhead();
    const uint32_t actual = MaxTenuringThreshold;
    const bool fixed = maxSet || (overrideSet && overrideValue != -1);
    const uint32_t expected = maxSet ? maximum : static_cast<uint32_t>(overrideValue);
    const bool thresholdMatches = fixed ? actual == expected :
        actual <= 15 && (actual == 15 || overhead * actual >= budget) &&
        (actual == 0 || overhead * (actual - 1) < budget);
    std::fprintf(stderr, "TENURING_INIT_TARGET actual=%u fixed=%d expected=%u overhead=%zu budget=%zu match=%d\n",
                 actual, fixed, expected, overhead, budget, thresholdMatches);
    GC_EXPECT_TRUE(thresholdMatches);
    GC_EXPECT_EQ(ZTenuringThreshold, overrideSet ? overrideValue : -1);
    const uint32_t selected = overrideSet && overrideValue != -1 ?
        static_cast<uint32_t>(overrideValue) : std::min(1u, actual);
    if (environment) {
        environmentSelected = selected;
        MRT_CjRuntimeStart(reinterpret_cast<void*>(&RunEnvironmentTenuringMain));
        _exit(2); // The product main must terminate with its own result.
    }
    {
        TenuringCollectionResult result;
        result.full = full;
        CJThreadHandle task = RunCJTask(CollectWithTenuringFlags, &result);
        GC_EXPECT_TRUE(task != nullptr);
        void* taskResult = nullptr;
        GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
        ReleaseHandle(task);
        CheckTenuringResult(result, selected);
    }
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

void CheckConflictingTenuringFlags(bool environment)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        RuntimeParam params{};
        params.heapParam.heapSize = 64 * 1024;
        params.coParam.processorNum = 1;
        params.gcParam.concGCThreads = 2;
        params.gcParam.maxTenuringThresholdSet = true;
        params.gcParam.maxTenuringThreshold = 4;
        params.gcParam.zTenuringThresholdSet = true;
        params.gcParam.zTenuringThreshold = 9;
        if (environment) {
            setenv("cjHeapSize", "64MB", 1);
            setenv("cjProcessorNum", "1", 1);
            setenv("cjConcGCThreads", "2", 1);
            setenv("cjMaxTenuringThreshold", "4", 1);
            setenv("cjZTenuringThreshold", "9", 1);
            MRT_CjRuntimeInit();
        } else {
            if (InitCJRuntime(&params) != E_OK) { _exit(125); }
        }
        std::fprintf(stderr, "TENURING_CONFLICT_ACCEPTED maximum=%u override=%d\n",
                     MaxTenuringThreshold, ZTenuringThreshold);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    // ZGC zArguments.cpp:188-191 rejects this combination at initialization.
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("ZTenuringThreshold must be within bounds of MaxTenuringThreshold") != std::string::npos;
    std::fprintf(stderr, "TENURING_CONFLICT_TARGET environment=%d status=%d rejected=%d\n",
                 environment, status, rejected);
    GC_EXPECT_TRUE(rejected);
}

}
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, DefaultSmallHeap) { CheckTenuringFlags(64 * 1024, 2, false, 0, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, DefaultLargeHeap) { CheckTenuringFlags(512 * 1024, 2, false, 0, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, DefaultCeiling) { CheckTenuringFlags(2048 * 1024, 2, false, 0, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, DefaultManyWorkers) { CheckTenuringFlags(64 * 1024, 8, false, 0, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, ExplicitAutomatic) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, -1); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, ExplicitMaximum) { CheckTenuringFlags(64 * 1024, 2, true, 12, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, MaximumZero) { CheckTenuringFlags(64 * 1024, 2, true, 0, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, OverrideZero) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, OverridePositive) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, 9); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, MaximumAndOverride) { CheckConflictingTenuringFlags(false); }

GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentOverride) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, 9, true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentMaximum) { CheckConflictingTenuringFlags(true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentAutomatic) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, -1, true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentZero) { CheckTenuringFlags(64 * 1024, 2, true, 0, true, 0, true); }

GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, MaximumOne) { CheckTenuringFlags(64 * 1024, 2, true, 1, false, 0); }

GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, PrecleanOverridesFlag) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, 9, false, true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, MaximumBoundary) { CheckTenuringFlags(64 * 1024, 2, true, 16, false, 0); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, OverrideBoundary) { CheckTenuringFlags(64 * 1024, 2, false, 0, true, 15); }

GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, DefaultBoundaryFourteen) { CheckTenuringFlags(112 * 1024, 1, false, 0, false, 0); }

GC_RUNTIME_OTHER_VM_TEST(TenuringGeometry, ConfiguredMaximumSurvivesInitialization)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 512 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t configured = params.heapParam.heapSize * 1024;
    const size_t effective = ZHeuristics::max_heap_size();
    const size_t medium = ZPageSizeMediumMax;
    const size_t budget = ZHeuristics::significant_young_overhead();
    std::fprintf(stderr, "TENURING_GEOMETRY_TARGET configured=%zu effective=%zu medium=%zu budget=%zu\n",
                 configured, effective, medium, budget);
    // The input identity and power-of-two tier are checked independently of
    // the headroom outputs used by the threshold tests above.
    GC_EXPECT_TRUE(effective == configured && medium == 16 * 1024 * 1024 && budget == configured / 4);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, OverrideEqualsMaximum) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, 4); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, OverrideBelowMaximum) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, 3); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentOverrideEqualsMaximum) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, 4, true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentOverrideBelowMaximum) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, 3, true); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, MaximumWithAutomatic) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, -1); }
GC_RUNTIME_OTHER_VM_TEST(TenuringFlags, EnvironmentMaximumWithAutomatic) { CheckTenuringFlags(64 * 1024, 2, true, 4, true, -1, true); }

// The debugger matrix observes real sampling and dispatch from this fixture.
// Its inputs are allocations and RuntimeParam, never precomputed rule results.
GC_RUNTIME_OTHER_VM_TEST(GcDirector, ProductCauseScenario)
{
    const char* scenario = std::getenv("GC_UNIT_CAUSE_SCENARIO");
    if (scenario == nullptr) scenario = "warmup";
    const bool highUsage = std::strcmp(scenario, "high_usage") == 0;
    const bool majorAllocationRate = std::strcmp(scenario, "major_allocation_rate") == 0;
    const bool allocationRate = majorAllocationRate || std::strncmp(scenario, "allocation_rate", 15) == 0;
    const bool proactive = std::strncmp(scenario, "proactive", 9) == 0;
    const bool timer = std::strstr(scenario, "timer") != nullptr;
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.backupGCInterval = timer ? 1 : 240;
    params.gcParam.concGCThreads = 2;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.staticGCThreads = std::strcmp(scenario, "allocation_rate_static") == 0;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    auto& manager = MutatorManager::Instance();
    manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    alignas(TypeInfo) unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    const size_t firstSize = heap.GetMaxCapacity() * (highUsage ? 15 : 8) / 16;
    type->SetInstanceSize(firstSize - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    {
        ScopedObjectAccess access;
        heap.RegisterExportRoot(MObject::NewPinnedObject(type, firstSize));
    }
    std::fprintf(stderr, "CAUSE_FIRST_ALLOCATION_READY scenario=%s\n", scenario);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (heap.old().CycleStats().Stats(TimeUtil::NanoSeconds()).warmupCycles < 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (majorAllocationRate) heap.RequestGC(GC_REASON_YOUNG);
    if (allocationRate || proactive) {
        const size_t nextSize = heap.GetMaxCapacity() * (allocationRate ? 7 : 2) / 16;
        alignas(TypeInfo) static unsigned char secondStorage[sizeof(TypeInfo)]{};
        auto* secondType = reinterpret_cast<TypeInfo*>(secondStorage);
        secondType->SetType(TypeKind::TYPE_KIND_CLASS);
        secondType->SetInstanceSize(nextSize - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(secondStorage), sizeof(secondStorage));
        {
            ScopedObjectAccess access;
            heap.RegisterExportRoot(MObject::NewPinnedObject(secondType, nextSize));
        }
        std::fprintf(stderr, "CAUSE_SECOND_ALLOCATION_READY scenario=%s\n", scenario);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto completed = heap.old().CycleStats().Stats(TimeUtil::NanoSeconds());
    manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_EQ(completed.warmupCycles, 3u);
}

namespace {
void CheckDriverCauseResult(GCReason cause, bool minor, bool clearSoft, bool preclean)
{
    auto* collected = ZCollectedHeap::heap();
    auto& heap = Heap::GetHeap();
    const auto youngBefore = heap.young().seqnum();
    const auto oldBefore = heap.old().seqnum();
    ZDriverPort& port = minor ? collected->driver_minor()->port() : collected->driver_major()->port();
    {
        ScopedEnterSaferegion safe(false);
        const ZDriverRequest request(cause, 2, minor ? 0 : 2);
        if (minor) collected->driver_minor()->collect(request);
        else collected->driver_major()->collect(request);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (port.is_busy() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Completion and policy assertions are independent: the target policy is
    // always printed, even if the request did not complete as expected.
    const bool done = !port.is_busy();
    const auto youngAfter = heap.young().seqnum();
    const auto oldAfter = heap.old().seqnum();
    const bool actualClear = heap.GetFinalizerProcessor().GetReferenceProcessor().uses_clear_all_soft_reference_policy();
    const auto expectedYoung = minor ? 1u : (preclean ? 2u : 1u);
    std::fprintf(stderr, "DRIVER_CAUSE_TARGET cause=%u minor=%d done=%d young=%llu old=%llu clear=%d expected_clear=%d expected_young=%u\n",
        cause, minor, done, static_cast<unsigned long long>(youngAfter - youngBefore),
        static_cast<unsigned long long>(oldAfter - oldBefore), actualClear, clearSoft, expectedYoung);
    GC_EXPECT_EQ(youngAfter - youngBefore, expectedYoung);
    GC_EXPECT_EQ(oldAfter - oldBefore, minor ? 0u : 1u);
    if (!minor) GC_EXPECT_EQ(actualClear, clearSoft);
    GC_EXPECT_TRUE(done);
}

void CheckDriverCause(GCReason cause, bool minor, bool clearSoft, bool preclean)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    CheckDriverCauseResult(cause, minor, clearSoft, preclean);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

struct PrecleanTask {
    GCReason cause;
    bool clearSoft;
    bool preclean;
    int result = 1;
};

void* CollectForPrecleanInvariant(void* argument)
{
    Mutator::GetMutator()->SetManagedContext(false);
    auto& task = *static_cast<PrecleanTask*>(argument);
    try {
        CheckDriverCauseResult(task.cause, false, task.clearSoft, task.preclean);
        task.result = 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PRECLEAN_TASK_ASSERT_FAILED %s\n", error.what());
    }
    return nullptr;
}

void CheckPrecleanWithoutShutdown(GCReason cause, bool clearSoft, bool preclean)
{
    // This fixture tests collection, not shutdown. The child completes a real
    // runtime task, then _exit skips shutdown (Debug native detach: #935).
    // Existing DriverCause fixtures still exercise FiniCJRuntime separately.
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        int result = 1;
        try {
            RuntimeParam params{};
            params.heapParam.heapSize = 64 * 1024;
            params.coParam.processorNum = 1;
            GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
            PrecleanTask input{cause, clearSoft, preclean};
            CJThreadHandle task = RunCJTask(CollectForPrecleanInvariant, &input);
            GC_EXPECT_TRUE(task != nullptr);
            void* taskResult = nullptr;
            GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
            ReleaseHandle(task);
            result = input.result;
            std::fprintf(stderr, "PRECLEAN_TASK_COMPLETED cause=%u result=%d shutdown=excluded\n", cause, result);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "PRECLEAN_TASK_SETUP_FAILED %s\n", error.what());
        }
        std::fflush(nullptr);
        _exit(result);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "PRECLEAN_CHILD_EXIT cause=%u status=%d\n", cause, status);
    GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
}

GC_RUNTIME_TEST(PrecleanWithoutShutdown, WhiteBox) { CheckPrecleanWithoutShutdown(GC_REASON_FORCE, true, true); }
GC_RUNTIME_TEST(PrecleanWithoutShutdown, Timer) { CheckPrecleanWithoutShutdown(GC_REASON_TIMER, false, false); }
GC_RUNTIME_TEST(PrecleanWithoutShutdown, AllocationStall) { CheckPrecleanWithoutShutdown(GC_REASON_ALLOCATION_STALL, true, true); }
GC_RUNTIME_TEST(PrecleanWithoutShutdown, User) { CheckPrecleanWithoutShutdown(GC_REASON_USER, false, true); }

GC_RUNTIME_OTHER_VM_TEST(DriverCause, MinorTimer) { CheckDriverCause(GC_REASON_TIMER, true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MinorAllocationRate) { CheckDriverCause(GC_REASON_ALLOCATION_RATE, true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MinorHighUsage) { CheckDriverCause(GC_REASON_HIGH_USAGE, true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MinorAllocationStall) { CheckDriverCause(GC_REASON_ALLOCATION_STALL, true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MinorWhiteBox) { CheckDriverCause(GC_REASON_YOUNG, true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorTimer) { CheckDriverCause(GC_REASON_TIMER, false, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorAllocationRate) { CheckDriverCause(GC_REASON_ALLOCATION_RATE, false, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorProactive) { CheckDriverCause(GC_REASON_PROACTIVE, false, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorWarmup) { CheckDriverCause(GC_REASON_WARMUP, false, false, false); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorAllocationStall) { CheckDriverCause(GC_REASON_ALLOCATION_STALL, false, true, true); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorWhiteBox) { CheckDriverCause(GC_REASON_FORCE, false, true, true); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorUser) { CheckDriverCause(GC_REASON_USER, false, false, true); }
GC_RUNTIME_OTHER_VM_TEST(DriverCause, MajorDiagnosticCommand) { CheckDriverCause(GC_REASON_DCMD_GC_RUN, false, false, true); }


#if defined(__OHOS__) && (__OHOS__ == 1)
// ProfilerAgent is an OHOS-only product entry (CangjieRuntimeApi.cpp).
GC_RUNTIME_OTHER_VM_TEST(DriverCause, ProfilerDiagnosticCommand)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    const auto before = heap.old().seqnum();
    bool response = false;
    ProfilerAgentImpl(R"({"id":1,"method":"HeapProfiler.collectGarbage"})",
        [&](const std::string&) { response = true; });
    const auto result = heap.old().seqnum();
    std::fprintf(stderr, "PROFILER_CAUSE_TARGET cause=%u expected=%u completed=%llu response=%d\n",
        ZDriver::major()->gc_cause(), GC_REASON_INVALID,
        static_cast<unsigned long long>(result - before), response);
    GC_EXPECT_EQ(ZDriver::major()->gc_cause(), GC_REASON_INVALID);
    GC_EXPECT_EQ(result - before, 1u);
    GC_EXPECT_TRUE(response);
}
#endif

namespace {
void CheckExternalRequestWorkers(GCReason cause)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 4;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 3;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& heap = Heap::GetHeap();
    const auto youngBefore = heap.young().seqnum();
    const auto oldBefore = heap.old().seqnum();
    heap.RequestGC(cause);
    const auto young = heap.young().seqnum();
    const auto old = heap.old().seqnum();
    const auto youngWorkers = heap.young().Workers()->active_workers();
    const auto oldWorkers = heap.old().Workers()->active_workers();
    std::fprintf(stderr,
        "REQUEST_WORKERS_TARGET cause=%u young_workers=%u old_workers=%u young_cycles=%llu old_cycles=%llu\n",
        cause, youngWorkers, oldWorkers,
        static_cast<unsigned long long>(young - youngBefore),
        static_cast<unsigned long long>(old - oldBefore));
    // The result of the product request, not a separately executed driver.
    GC_EXPECT_EQ(youngWorkers, 2u);
    if (cause != GC_REASON_YOUNG) GC_EXPECT_EQ(oldWorkers, 3u);
    GC_EXPECT_EQ(young - youngBefore, cause == GC_REASON_YOUNG ? 1u : 2u);
    GC_EXPECT_EQ(old - oldBefore, cause == GC_REASON_YOUNG ? 0u : 1u);
}
}

GC_RUNTIME_OTHER_VM_TEST(RequestWorkers, ExternalYoung) { CheckExternalRequestWorkers(GC_REASON_YOUNG); }
GC_RUNTIME_OTHER_VM_TEST(RequestWorkers, ExternalUser) { CheckExternalRequestWorkers(GC_REASON_USER); }
GC_RUNTIME_OTHER_VM_TEST(RequestWorkers, ExternalForce) { CheckExternalRequestWorkers(GC_REASON_FORCE); }
GC_RUNTIME_OTHER_VM_TEST(RequestWorkers, ExternalDiagnostic) { CheckExternalRequestWorkers(GC_REASON_DCMD_GC_RUN); }

// ZGC zHeuristics.cpp:114-116 and zArguments.cpp:160-174: the same flag
// supplies the young budget and the initialization-time tenuring bound.
GC_RUNTIME_OTHER_VM_TEST(YoungCompactionLimit, BudgetUsesFlag)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t configured = params.heapParam.heapSize * 1024;
    const size_t expected = static_cast<size_t>(configured * (ZYoungCompactionLimit / 100));
    const size_t actual = ZHeuristics::significant_young_overhead();
    std::fprintf(stderr, "YOUNG_BUDGET_TARGET flag=%.1f heap=%zu expected=%zu actual=%zu\n",
                 ZYoungCompactionLimit, configured, expected, actual);
    GC_EXPECT_EQ(actual, expected);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(YoungCompactionLimit, InitializationUsesFlagBudget)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t configured = params.heapParam.heapSize * 1024;
    const size_t budget = static_cast<size_t>(configured * (ZYoungCompactionLimit / 100));
    const size_t perAge = ZHeuristics::relocation_headroom();
    const uint32_t actual = MaxTenuringThreshold;
    const bool matches = actual <= 15 && (actual == 15 || perAge * actual >= budget) &&
                         (actual == 0 || perAge * (actual - 1) < budget);
    std::fprintf(stderr, "YOUNG_INIT_TARGET flag=%.1f budget=%zu per_age=%zu actual=%u matches=%d\n",
                 ZYoungCompactionLimit, budget, perAge, actual, matches);
    GC_EXPECT_TRUE(matches);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC zDriver.cpp:76-89,124,325: constructor registration identifies the
// product-owned drivers before their thread starts.
GC_RUNTIME_OTHER_VM_TEST(DriverRegistration, ProductOwnedMinor)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto* registered = ZDriver::minor();
    auto* owned = ZCollectedHeap::heap()->driver_minor();
    std::fprintf(stderr, "REGISTRATION_TARGET minor registered=%p owned=%p\n",
        static_cast<void*>(registered), static_cast<void*>(owned));
    GC_EXPECT_TRUE(registered == owned);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(DriverRegistration, ProductOwnedMajor)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto* registered = ZDriver::major();
    auto* owned = ZCollectedHeap::heap()->driver_major();
    std::fprintf(stderr, "REGISTRATION_TARGET major registered=%p owned=%p\n",
        static_cast<void*>(registered), static_cast<void*>(owned));
    GC_EXPECT_TRUE(registered == owned);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct GenerationStateResult {
    uint32_t youngBefore = 0, youngAfter = 0, oldBefore = 0, oldAfter = 0;
};
void* CollectForGenerationState(void* context)
{
    auto& result = *static_cast<GenerationStateResult*>(context);
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    constexpr size_t size = 1024;
    type->SetInstanceSize(size - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    std::vector<U64> roots;
    const size_t count = 3 * ZPageSizeSmall / size;
    for (size_t i = 0; i < count; ++i) {
        // Populate three old allocation pages using the product allocator.
        // Keep one object per page so the real selector can reclaim pages.
        const auto address = heap.object_allocator().alloc_for_relocation(size, PageAge::old);
        auto* object = reinterpret_cast<BaseObject*>(address);
        object->SetClassInfo(type);
        if (i % (ZPageSizeSmall / size) == 0) roots.push_back(heap.RegisterExportRoot(object));
    }
    mutator->SetManagedContext(false);
    result.youngBefore = heap.young().seqnum();
    result.oldBefore = heap.old().seqnum();
    heap.RequestGC(GC_REASON_USER);
    heap.RequestGC(GC_REASON_USER);
    result.youngAfter = heap.young().seqnum();
    result.oldAfter = heap.old().seqnum();
    for (U64 root : roots) heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(GenerationState, ProductSequenceAndForwarding)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    GenerationStateResult result;
    CJThreadHandle task = RunCJTask(CollectForGenerationState, &result);
    GC_EXPECT_TRUE(task != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "GENERATION_SEQUENCE_TARGET young=%u->%u old=%u->%u\n",
        result.youngBefore, result.youngAfter, result.oldBefore, result.oldAfter);
    GC_EXPECT_EQ(result.youngAfter, result.youngBefore + 4);
    GC_EXPECT_EQ(result.oldAfter, result.oldBefore + 2);
    GC_EXPECT_TRUE(Heap::GetHeap().young().is_phase_relocate());
    GC_EXPECT_TRUE(Heap::GetHeap().old().is_phase_relocate());
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

extern "C" bool CJ_MCC_IsGCRunning();
GC_RUNTIME_OTHER_VM_TEST(GenerationState, DriverActivityABI)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const bool before = CJ_MCC_IsGCRunning();
    CheckDriverCauseResult(GC_REASON_TIMER, false, false, false);
    const bool after = CJ_MCC_IsGCRunning();
    std::fprintf(stderr, "GENERATION_ACTIVITY_IDLE_TARGET before=%d after=%d\n", before, after);
    GC_EXPECT_FALSE(before);
    GC_EXPECT_FALSE(after);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}


namespace {
void CheckSoftMax(size_t heapKB, const char* configured, size_t softKB, bool softSet,
                  size_t expected, bool managed = false)
{
    if (configured != nullptr) {
        setenv("cjSoftMaxHeapSize", configured, 1);
    } else {
        unsetenv("cjSoftMaxHeapSize");
    }
    RuntimeParam params{};
    params.heapParam.heapSize = heapKB;
    params.heapParam.softHeapSize = softKB;
    params.heapParam.softHeapSizeSet = softSet;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    pid_t child = -1;
    if (managed) {
        child = fork();
        GC_EXPECT_TRUE(child >= 0);
        if (child != 0) {
            int status = 0;
            GC_EXPECT_EQ(waitpid(child, &status, 0), child);
            GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            return;
        }
        if (heapKB == 0) {
            unsetenv("cjHeapSize");
        } else {
            setenv("cjHeapSize", (std::to_string(heapKB) + "KB").c_str(), 1);
        }
        setenv("cjProcessorNum", "1", 1);
        setenv("cjConcGCThreads", "2", 1);
        MRT_CjRuntimeInit();
    } else {
        GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    }
    const size_t maximum = ZHeuristics::max_heap_size();
    const size_t soft = Heap::GetHeap().soft_max_capacity();
    std::fprintf(stderr, "SOFT_MAX_TARGET configured=%s max=%zu soft=%zu expected=%zu\n",
                 configured == nullptr ? "default" : configured, maximum, soft, expected);
    const size_t expectedMaximum = heapKB != 0 ? heapKB * KB :
        CangjieRuntime::GetHeapParam().heapSize * KB;
    // expected=0 denotes the all-default ergonomics case, not explicit zero.
    const size_t expectedSoft = expected == 0 ? maximum * 90 / 100 : expected;
    const bool hardMatches = maximum == expectedMaximum;
    const bool softMatches = soft == expectedSoft;
    std::fprintf(stderr, "SOFT_MAX_ASSERT hard_match=%d soft_match=%d expected_soft=%zu\n",
                 hardMatches, softMatches, expectedSoft);
    if (managed) {
        _exit(hardMatches && softMatches ? 0 : 1);
    }
    GC_EXPECT_TRUE(hardMatches);
    GC_EXPECT_TRUE(softMatches);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ExplicitEnvironment)
{
    CheckSoftMax(512 * 1024, "128M", 0, false, size_t(128) * MB);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ExplicitHardLimit)
{
    CheckSoftMax(512 * 1024, "512M", 0, false, size_t(512) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, DefaultErgonomics)
{
    CheckSoftMax(0, nullptr, 0, false, 0);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ExplicitMaxEqualsDefault)
{
    CheckSoftMax(64 * 1024, nullptr, 0, false, size_t(64) * MB);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ExplicitMax512)
{
    CheckSoftMax(512 * 1024, nullptr, 0, false, size_t(512) * MB);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ExplicitParameter)
{
    CheckSoftMax(512 * 1024, nullptr, 128 * 1024, true, size_t(128) * MB);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, EnvironmentPrecedesParameter)
{
    CheckSoftMax(512 * 1024, "256M", 128 * 1024, true, size_t(256) * MB);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ManagedDefaultErgonomics)
{
    CheckSoftMax(0, nullptr, 0, false, 0, true);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ManagedExplicitMax)
{
    CheckSoftMax(256 * 1024, nullptr, 0, false, size_t(256) * MB, true);
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxHeapSize, ManagedExplicitSoft)
{
    CheckSoftMax(0, "128M", 0, false, size_t(128) * MB, true);
}

namespace {
void CheckSoftConfig(size_t softKB, const char* env, RTErrorCode expected, bool explicitZero = false)
{
    if (env == nullptr) {
        unsetenv("cjSoftMaxHeapSize");
    } else {
        setenv("cjSoftMaxHeapSize", env, 1);
    }
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.heapParam.softHeapSize = softKB;
    params.heapParam.softHeapSizeSet = explicitZero || softKB != 0;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    const RTErrorCode actual = InitCJRuntime(&params);
    std::fprintf(stderr, "SOFT_CONSTRAINT_ASSERT soft_kb=%zu env=%s actual=%d expected=%d\n",
                 softKB, env == nullptr ? "unset" : env, actual, expected);
    GC_EXPECT_EQ(actual, expected);
    if (actual == E_OK) {
        GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    }
}
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, BelowMaximum)
{
    CheckSoftConfig(32 * 1024, nullptr, E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EqualsMaximum)
{
    CheckSoftConfig(64 * 1024, nullptr, E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, ExplicitZero)
{
    CheckSoftConfig(0, nullptr, E_OK, true);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, AboveMaximum)
{
    CheckSoftConfig(128 * 1024, nullptr, E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, ParameterOverflow)
{
    CheckSoftConfig(std::numeric_limits<size_t>::max() / KB + 1, nullptr, E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentAboveMaximum)
{
    CheckSoftConfig(32 * 1024, "128M", E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentOverridesInvalidParameter)
{
    CheckSoftConfig(128 * 1024, "32M", E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentOverridesOverflowParameter)
{
    CheckSoftConfig(std::numeric_limits<size_t>::max(), "32M", E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentZero)
{
    CheckSoftConfig(0, "0K", E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentByteOverflow)
{
    CheckSoftConfig(0, "18014398509481984K", E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentUnitOverflow)
{
    CheckSoftConfig(0, "17592186044416G", E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, EnvironmentNumberOverflow)
{
    CheckSoftConfig(0, "18446744073709551616K", E_ARGS);
}

namespace {
void CheckManagedSoftConstraint(const char* soft, const char* diagnostic)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        setenv("cjHeapSize", "64MB", 1);
        setenv("cjProcessorNum", "1", 1);
        setenv("cjConcGCThreads", "2", 1);
        setenv("cjSoftMaxHeapSize", soft, 1);
        MRT_CjRuntimeInit();
        std::fprintf(stderr, "SOFT_CONSTRAINT_ACCEPTED soft=%s\n", soft);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find(diagnostic) != std::string::npos;
    std::fprintf(stderr, "SOFT_MANAGED_CONSTRAINT_ASSERT soft=%s status=%d rejected=%d\n", soft, status, rejected);
    GC_EXPECT_TRUE(rejected);
}
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, ManagedAboveMaximum)
{
    CheckManagedSoftConstraint("128M", "SoftMaxHeapSize must be less than or equal");
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, ManagedByteOverflow)
{
    CheckManagedSoftConstraint("18014398509481984K", "Invalid cjSoftMaxHeapSize");
}
GC_RUNTIME_OTHER_VM_TEST(SoftMaxConstraint, ManagedUnitOverflow)
{
    CheckManagedSoftConstraint("17592186044416G", "Invalid cjSoftMaxHeapSize");
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, Decimal)
{
    CheckSoftMax(64 * 1024, "40M", 0, false, size_t(40) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, LeadingZeroDecimal)
{
    CheckSoftMax(64 * 1024, "040M", 0, false, size_t(40) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, DecimalEight)
{
    CheckSoftMax(64 * 1024, "8M", 0, false, size_t(8) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, LeadingZeroEight)
{
    CheckSoftMax(64 * 1024, "08M", 0, false, size_t(8) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, Hexadecimal)
{
    CheckSoftMax(64 * 1024, "0x20M", 0, false, size_t(32) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, UppercaseHexadecimal)
{
    CheckSoftMax(64 * 1024, "0X20M", 0, false, size_t(32) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, DecimalHexControl)
{
    CheckSoftMax(64 * 1024, "32M", 0, false, size_t(32) * MB);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, AboveMaximum)
{
    CheckSoftConfig(0, "100M", E_ARGS);
}

GC_RUNTIME_OTHER_VM_TEST(SoftMaxRadix, LeadingZeroAboveMaximum)
{
    CheckSoftConfig(0, "0100M", E_ARGS);
}

// HotSpot test_arguments.cpp:67-166 and INTEGER_TEST_TABLE unsigned size_t column.
#include "Heap/shared/gcArguments.hpp"
namespace {
constexpr size_t k = 1024;
constexpr size_t m = k * k;
constexpr size_t g = m * k;
constexpr size_t t = g * k;
void CheckMemoryParser(const char* input, bool accepted, size_t expected, int referenceLine)
{
    size_t parsed = 4711;
    const auto rc = GCArguments::parse_memory_size(input, &parsed, 0,
                                                  std::numeric_limits<size_t>::max());
    std::fprintf(stderr, "MEMORY_TABLE input=[%s] reference_line=%d rc=%d accepted=%d bytes=%zu expected=%zu\n",
                 input, referenceLine, rc, accepted, parsed, expected);
    GC_EXPECT_EQ(rc, accepted ? GCArguments::arg_in_range : GCArguments::arg_unreadable);
    if (accepted) GC_EXPECT_EQ(parsed, expected);
}
void CheckMemoryRuntime(const char* input, bool accepted, size_t expected)
{
    setenv("cjSoftMaxHeapSize", input, 1);
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    const auto rc = InitCJRuntime(&params);
    const bool valid = accepted && expected <= 64 * MB;
    std::fprintf(stderr, "MEMORY_RUNTIME input=[%s] rc=%d valid=%d expected=%zu\n", input, rc, valid, expected);
    GC_EXPECT_EQ(rc, valid ? E_OK : E_ARGS);
    if (rc == E_OK) {
        const size_t actual = Heap::GetHeap().soft_max_capacity();
        std::fprintf(stderr, "MEMORY_CONSUMER actual=%zu expected=%zu\n", actual, expected);
        GC_EXPECT_EQ(actual, expected);
        GC_EXPECT_EQ(ZHeuristics::max_heap_size(), 64 * MB);
        GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    }
}
}
#define SOFT_MEMORY_CASE(id, input, accepted, bytes, line) \
    GC_TEST(SoftMemoryTable, id) { CheckMemoryParser(input, accepted, bytes, line); } \
    GC_RUNTIME_OTHER_VM_TEST(SoftMemoryRuntime, id) { CheckMemoryRuntime(input, accepted, bytes); }
#include "soft_memory_cases.inc"
#undef SOFT_MEMORY_CASE

GC_TEST(SoftMemoryTable, MemoryRangeBounds)
{
    constexpr size_t max_uintx = std::numeric_limits<size_t>::max();
    constexpr size_t max_intx = std::numeric_limits<intptr_t>::max();
    struct RangeCase { size_t value; size_t minimum; size_t maximum; GCArguments::ArgsRange expected; };
    const RangeCase cases[] = {
        { 999,  1000, max_uintx, GCArguments::arg_too_small },
        { 1000, 1000, max_uintx, GCArguments::arg_in_range },
        { 1001, 1000, max_uintx, GCArguments::arg_in_range },
        { max_intx - 2, max_intx - 1, max_uintx, GCArguments::arg_too_small },
        { max_intx - 1, max_intx - 1, max_uintx, GCArguments::arg_in_range },
        { max_intx - 0, max_intx - 1, max_uintx, GCArguments::arg_in_range },
        { max_intx - 1, max_intx, max_uintx, GCArguments::arg_too_small },
        { max_intx    , max_intx, max_uintx, GCArguments::arg_in_range },
        { max_uintx - 2, max_uintx - 1, max_uintx, GCArguments::arg_too_small },
        { max_uintx - 1, max_uintx - 1, max_uintx, GCArguments::arg_in_range },
        { max_uintx    , max_uintx - 1, max_uintx, GCArguments::arg_in_range },
        { max_uintx - 1, max_uintx, max_uintx, GCArguments::arg_too_small },
        { max_uintx    , max_uintx, max_uintx, GCArguments::arg_in_range },
        { max_uintx - 1, 1000, max_uintx, GCArguments::arg_in_range },
        { max_uintx    , 1000, max_uintx, GCArguments::arg_in_range },
        { max_intx - 2     , 1000, max_intx - 1, GCArguments::arg_in_range },
        { max_intx - 1     , 1000, max_intx - 1, GCArguments::arg_in_range },
        { max_intx         , 1000, max_intx - 1, GCArguments::arg_too_big },
        { max_intx - 1     , 1000, max_intx, GCArguments::arg_in_range },
        { max_intx         , 1000, max_intx, GCArguments::arg_in_range },
    };
    size_t parsedRangeValue = 0;
    for (const auto& entry : cases) {
        const std::string input = std::to_string(entry.value);
        const auto actual = GCArguments::parse_memory_size(input.c_str(), &parsedRangeValue,
                                                           entry.minimum, entry.maximum);
        std::fprintf(stderr, "MEMORY_RANGE value=%zu min=%zu max=%zu actual=%d expected=%d\n",
                     entry.value, entry.minimum, entry.maximum, actual, entry.expected);
        GC_EXPECT_EQ(actual, entry.expected);
    }
}
