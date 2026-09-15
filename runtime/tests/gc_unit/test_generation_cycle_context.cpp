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
#include <array>
#include <cstring>
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"


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
    ZWorkers& youngWorkers = resources.GetWorkers(GCCycleGeneration::YOUNG);
    ZWorkers& oldWorkers = resources.GetWorkers(GCCycleGeneration::OLD);
    // ZWorkers (zWorkers.cpp:60-64) initializes each generation with all
    // concurrent workers active, and no generation is active before a cycle.
    Expect(youngWorkers.active_workers() == concurrent && oldWorkers.active_workers() == concurrent,
           "worker_startup_active_budget");
    Expect(!youngWorkers.is_active() && !oldWorkers.is_active(), "worker_startup_inactive");
    std::printf("WORKER_INPUT cpu=%zu heap=%zu region=%zu concurrent=%zu\n",
                cpuCount, heapBytes, regionBytes, concurrent);
#if defined(MRT_TESTABLE_INTERNALS)
    auto& tracing = static_cast<TracingCollector&>(collector);
    unsigned youngLabels = 0;
    unsigned oldLabels = 0;
    unsigned combinedMarkStarts = 0;
    unsigned minorMarkStarts = 0;
    GCCycleSnapshot preludeOld {};
    uintptr_t preludeOldColor = 0;
    // Port the VM_ZMarkStartYoungAndOld/VM_ZMarkStartYoung phase invariants
    // (zGeneration.cpp:583-659) through the real driver request below.
    tracing.testYoungMarkStarted = [&]() {
        const auto young = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
        const auto old = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
        Expect(young.active, "young_mark_start_active");
        if (collector.GetGenerationCycle(GCCycleGeneration::YOUNG).IsMajorRoots()) {
            ++combinedMarkStarts;
            preludeOld = old;
            preludeOldColor = ::g_cjMarkBadMask & ZPointerMarkedOldMask;
            Expect(old.active && old.phase == GC_PHASE_ENUM && old.reason == GC_REASON_USER,
                   "prelude_starts_old_mark");
            StoreBarrierBuffer buffer;
            RootSlot slot;
            buffer.Add(reinterpret_cast<MAddress>(&slot), zpointer::null, Heap::GetHeap().GetRememberedSet());
            Expect((buffer.LastProcessedColorForTest() & ZPointerMarkedOldMask) == (::g_cjStoreGoodMask & ZPointerMarkedOldMask), "prelude_store_buffer_old_obligation");
            buffer.Discard();
        } else {
            ++minorMarkStarts;
            Expect(!old.active, "independent_minor_does_not_start_old");
        }
    };
    tracing.testCyclePrepared = [&]() {
        // The real driver has selected and prepared its cycle. Add captures
        // its own state; the test does not provide a phase or generation.
        StoreBarrierBuffer buffer;
        RootSlot slot;
        buffer.Add(reinterpret_cast<MAddress>(&slot), zpointer::null, Heap::GetHeap().GetRememberedSet());
        const auto storedColor = buffer.LastProcessedColorForTest();
        const bool young = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).active;
        ZWorkers& current = resources.GetWorkers(young ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
        ZWorkers& other = resources.GetWorkers(young ? GCCycleGeneration::OLD : GCCycleGeneration::YOUNG);
        std::printf("WORKER_PREPARED generation=%s active=%u other_active=%u workers=%u\n",
                    young ? "young" : "old", current.is_active(), other.is_active(), current.active_workers());
        Expect(current.active_workers() == concurrent, "worker_prepared_concurrent_budget");
        Expect(current.is_active(), young ? "worker_young_active_during_cycle" : "worker_old_active_during_cycle");
        Expect(!other.is_active(), "worker_other_inactive_during_cycle");
        if (young) {
            ++youngLabels;
            Expect(storedColor == static_cast<uintptr_t>(::g_cjStoreGoodMask), "store_buffer_young_color");
        } else {
            ++oldLabels;
            const auto old = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
            Expect(old.sequence == preludeOld.sequence && old.requestIndex == preludeOld.requestIndex,
                   "old_body_keeps_prelude_identity");
            Expect((::g_cjMarkBadMask & ZPointerMarkedOldMask) == preludeOldColor,
                   "old_body_keeps_prelude_color");
            Expect(storedColor == static_cast<uintptr_t>(::g_cjStoreGoodMask), "store_buffer_old_color");
        }
        buffer.Discard();
    };
#endif
    auto y0 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o0 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    collector.RequestGC(GC_REASON_USER, false);
    Expect(youngWorkers.active_workers() == concurrent && oldWorkers.active_workers() == concurrent,
           "worker_major_phase_budget");
    Expect(!youngWorkers.is_active() && !oldWorkers.is_active(), "worker_major_completion");
    // ZStatCycle::at_end (zStat.cpp:1252-1253) reset the old generation's
    // worker accounting at the end of the major; a minor must not add to it.
    const auto oldWorkerStats1 = collector.GetGenerationCycle(GCCycleGeneration::OLD).StatWorkers()->stats();
    auto y1 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o1 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y1.sequence == y0.sequence + 1, "major_prelude_young_sequence");
    Expect(o1.sequence == o0.sequence + 1, "major_old_sequence");
    Expect(o1.reason == GC_REASON_USER && !o1.active, "major_reason_completion");
    collector.RequestGC(GC_REASON_YOUNG, false);
    Expect(youngWorkers.active_workers() == concurrent && !youngWorkers.is_active(), "worker_minor_phase_budget");
    const auto oldWorkerStats2 = collector.GetGenerationCycle(GCCycleGeneration::OLD).StatWorkers()->stats();
    Expect(oldWorkerStats2._accumulated_duration == oldWorkerStats1._accumulated_duration &&
           oldWorkerStats2._accumulated_time == oldWorkerStats1._accumulated_time &&
           oldWorkers.active_workers() == concurrent && !oldWorkers.is_active(), "worker_minor_preserves_old");
    auto y2 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o2 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y2.sequence == y1.sequence + 1, "minor_sequence");
    Expect(Same(o1, o2), "minor_preserves_old_state");
    Expect(y2.reason == GC_REASON_YOUNG && !y2.active, "minor_reason_completion");
    Expect(y2.phase == GC_PHASE_RECLAIM_SATB_NODE, "minor_phase_consumer");
    std::printf("PRODUCT_STATE young_seq=%llu old_seq=%llu young_phase=%u old_phase=%u\n",
        (unsigned long long)y2.sequence, (unsigned long long)o2.sequence,
        (unsigned)y2.phase, (unsigned)o2.phase);
#if defined(MRT_TESTABLE_INTERNALS)
    Expect(youngLabels == 2 && oldLabels == 1, "store_buffer_real_cycle_inputs");
    Expect(combinedMarkStarts == 1 && minorMarkStarts == 1, "real_mark_start_variants_observed");
    tracing.testYoungMarkStarted = nullptr;
    tracing.testCyclePrepared = nullptr;
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
