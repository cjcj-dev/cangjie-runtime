// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <sched.h>
#include <algorithm>
#include <set>
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Cangjie.h"
#include "Heap/Heap.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Mutator/SatbBuffer.h"

using namespace MapleRuntime;
namespace {
unsigned failures = 0;
void Expect(bool result, const char* name)
{
    std::printf("ASSERT %s %s\n", name, result ? "PASS" : "FAIL");
    std::fflush(stdout);
    failures += !result;
}
bool Same(const GCCycleSnapshot& a, const GCCycleSnapshot& b)
{
    return a.generation == b.generation && a.sequence == b.sequence &&
        a.requestIndex == b.requestIndex && a.reason == b.reason &&
        a.phase == b.phase && a.active == b.active;
}
void* Exercise(void*)
{
    Collector& collector = Heap::GetHeap().GetCollector();
    auto& resources = Heap::GetHeap().GetCollectorResources();
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    const int affinityRc = sched_getaffinity(0, sizeof(cpus), &cpus);
    Expect(affinityRc == 0, "worker_cpu_input");
    const size_t cpuCount = CPU_COUNT(&cpus);
    const size_t heapBytes = Heap::GetHeap().GetMaxCapacity();
    const size_t regionBytes = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator())
        .GetRegionManager().GetThreadLocalRegionSize();
    const size_t heapLimit = heapBytes / regionBytes / 50;
    const size_t concurrent = std::max<size_t>(1, std::min((cpuCount + 3) / 4, heapLimit));
    const size_t parallel = std::max<size_t>(1, std::min((cpuCount * 3 + 4) / 5, heapLimit));
    Expect(resources.GetGCThreadCount(true) == static_cast<int>(concurrent), "worker_concurrent_budget");
    Expect(resources.GetGCThreadCount(false) == static_cast<int>(parallel), "worker_parallel_budget");
    auto youngWorkers0 = resources.GetWorkers(GCCycleGeneration::YOUNG).GetSnapshot();
    auto oldWorkers0 = resources.GetWorkers(GCCycleGeneration::OLD).GetSnapshot();
    Expect(youngWorkers0.capacity == parallel && oldWorkers0.capacity == parallel, "worker_generation_capacity");
    std::printf("WORKER_INPUT cpu=%zu heap=%zu region=%zu concurrent=%zu parallel=%zu\n",
                cpuCount, heapBytes, regionBytes, concurrent, parallel);
#if defined(MRT_TESTABLE_INTERNALS)
    auto& tracing = static_cast<TracingCollector&>(collector);
    unsigned youngLabels = 0;
    unsigned oldLabels = 0;
    unsigned rootResults = 0;
    tracing.testCyclePrepared = [&]() {
        // The real driver has selected and prepared its cycle. Add captures
        // its own state; the test does not provide a phase or generation.
        StoreBarrierBuffer buffer;
        RootSlot slot;
        buffer.Add(reinterpret_cast<MAddress>(&slot), zpointer::null, Heap::GetHeap().GetRememberedSet());
        const auto stored = buffer.LastInstalledStateForTest();
        const bool young = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).active;
        if (young) {
            ++youngLabels;
            Expect(stored.youngMark, "store_buffer_young_label");
        } else {
            ++oldLabels;
            Expect(!stored.youngMark, "store_buffer_old_label");
        }
        Expect(stored.phase == collector.GetGCPhase(), "store_buffer_phase_label");
        buffer.Discard();
    };
    tracing.testRootsResult = [&](GCWorkers::Generation generation, TracingCollector::RootSet& result) {
        std::set<BaseObject*> observed;
        for (auto* node = result.head(); node != nullptr; node = node->next) {
            auto copy = *node;
            while (!copy.empty()) {
                observed.insert(copy.back().object());
                copy.pop_back();
            }
        }
        size_t expected = 0;
        bool included = true;
        Heap::GetHeap().VisitStaticRoots([&](RootSlot& slot) {
            auto* object = to_object(safe(slot.LoadPlain()));
            if (object != nullptr && Heap::IsHeapAddress(object)) {
                ++expected;
                included = included && observed.count(object) != 0;
            }
        });
        ++rootResults;
        std::printf("ROOT_RESULT expected_static=%zu observed_objects=%zu generation=%u\n",
                    expected, observed.size(), static_cast<unsigned>(generation));
        Expect(expected > 0, "worker_root_witness_exists");
        Expect(included, "worker_root_result_contains_statics");
        Expect(generation == GCWorkers::Generation::OLD, "worker_root_result_owner");
    };
#endif
    auto y0 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o0 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    collector.RequestGC(GC_REASON_USER, false);
    auto youngWorkers1 = resources.GetWorkers(GCCycleGeneration::YOUNG).GetSnapshot();
    auto oldWorkers1 = resources.GetWorkers(GCCycleGeneration::OLD).GetSnapshot();
    Expect(youngWorkers1.activeWorkers == parallel && oldWorkers1.activeWorkers == concurrent,
           "worker_major_phase_budget");
    Expect(!youngWorkers1.cycleActive && !oldWorkers1.cycleActive, "worker_major_completion");
    auto y1 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o1 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y1.sequence == y0.sequence + 1, "major_prelude_young_sequence");
    Expect(o1.sequence == o0.sequence + 1, "major_old_sequence");
    Expect(o1.reason == GC_REASON_USER && !o1.active, "major_reason_completion");
    Expect(SatbBuffer::Instance().GetGeneration() == GCCycleGeneration::OLD, "major_satb_owner");
    collector.RequestGC(GC_REASON_YOUNG, false);
    auto youngWorkers2 = resources.GetWorkers(GCCycleGeneration::YOUNG).GetSnapshot();
    auto oldWorkers2 = resources.GetWorkers(GCCycleGeneration::OLD).GetSnapshot();
    Expect(youngWorkers2.activeWorkers == parallel && !youngWorkers2.cycleActive, "worker_minor_phase_budget");
    Expect(oldWorkers2.batch == oldWorkers1.batch && oldWorkers2.activeWorkers == oldWorkers1.activeWorkers &&
           oldWorkers2.cycleActive == oldWorkers1.cycleActive, "worker_minor_preserves_old");
    auto y2 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o2 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y2.sequence == y1.sequence + 1, "minor_sequence");
    Expect(Same(o1, o2), "minor_preserves_old_state");
    Expect(y2.reason == GC_REASON_YOUNG && !y2.active, "minor_reason_completion");
    Expect(y2.phase == GC_PHASE_RECLAIM_SATB_NODE, "minor_phase_consumer");
    Expect(SatbBuffer::Instance().GetGeneration() == GCCycleGeneration::YOUNG, "minor_satb_owner");
    std::printf("PRODUCT_STATE young_seq=%llu old_seq=%llu young_phase=%u old_phase=%u\n",
        (unsigned long long)y2.sequence, (unsigned long long)o2.sequence,
        (unsigned)y2.phase, (unsigned)o2.phase);
#if defined(MRT_TESTABLE_INTERNALS)
    Expect(youngLabels == 2 && oldLabels == 1, "store_buffer_real_cycle_inputs");
    Expect(rootResults > 0, "worker_root_result_observed");
    tracing.testCyclePrepared = nullptr;
    tracing.testRootsResult = nullptr;
#endif
    return reinterpret_cast<void*>(static_cast<uintptr_t>(failures));
}
}
// Called from a compiler-built managed frame so collection sees real stack maps.
extern "C" int cycleExercise()
{
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&InitCJRuntime), &info)) {
        std::printf("PRODUCT_LOADED %s\n", info.dli_fname);
    }
    return static_cast<int>(reinterpret_cast<uintptr_t>(Exercise(nullptr)));
}
