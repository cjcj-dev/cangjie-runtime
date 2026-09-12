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
#include "Heap/Barrier/StoreBarrierBuffer.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Heap.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
// Seed inputs of the real processor/collector; never call or reconstruct the
// enum task in the test. The real major request owns execution and merging.
// This fixture checks root scanning, not finalizer scheduling or invocation.
struct GenerationCycleRootTestAccess {
    static void Install(TracingCollector& collector, const std::array<BaseObject*, 6>& objects)
    {
        auto& processor = Heap::GetHeap().GetFinalizerProcessor();
        {
            std::lock_guard<std::mutex> lock(processor.listLock);
            RootSlot queued, working;
            StorePlain(queued, from_object(objects[0]));
            StorePlain(working, from_object(objects[1]));
            processor.finalizables.push_back(queued);
            processor.workingFinalizables.push_back(working);
            // Deliberately do not schedule finalization: only the scanner's
            // queued/working input branches are under test.
        }
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            collector.resurrectedExportObjectes.insert(objects[2]);
            collector.resurrectedExportObjectesForwardPhase.insert(objects[3]);
        }
        {
            std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
            collector.cycleRefWorkStack[objects[4]].push_back(objects[5]);
        }
    }
    static void Remove(TracingCollector& collector, const std::array<BaseObject*, 6>& objects)
    {
        auto& processor = Heap::GetHeap().GetFinalizerProcessor();
        {
            std::lock_guard<std::mutex> lock(processor.listLock);
            auto remove = [&](ManagedList<RootSlot>& roots, BaseObject* object) {
                for (auto it = roots.begin(); it != roots.end();) {
                    if (to_object(safe(it->LoadPlain())) == object) it = roots.erase(it);
                    else ++it;
                }
            };
            remove(processor.finalizables, objects[0]);
            remove(processor.workingFinalizables, objects[1]);
        }
        {
            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
            collector.resurrectedExportObjectes.erase(objects[2]);
            collector.resurrectedExportObjectesForwardPhase.erase(objects[3]);
        }
        {
            std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
            collector.cycleRefWorkStack.erase(objects[4]);
        }
    }
};
}
#endif

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
    Expect(!youngWorkers0.cycleActive && !oldWorkers0.cycleActive, "worker_startup_inactive");
    std::printf("WORKER_INPUT cpu=%zu heap=%zu region=%zu concurrent=%zu parallel=%zu\n",
                cpuCount, heapBytes, regionBytes, concurrent, parallel);
#if defined(MRT_TESTABLE_INTERNALS)
    auto& tracing = static_cast<TracingCollector&>(collector);
    unsigned youngLabels = 0;
    unsigned oldLabels = 0;
    unsigned rootResults = 0;
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
        if (resources.YoungPreludeRequest() != nullptr) {
            ++combinedMarkStarts;
            preludeOld = old;
            preludeOldColor = ::g_cjMarkBadMask & MARKED_OLD_MASK;
            Expect(old.active && old.phase == GC_PHASE_ENUM && old.reason == GC_REASON_USER,
                   "prelude_starts_old_mark");
            StoreBarrierBuffer buffer;
            RootSlot slot;
            buffer.Add(reinterpret_cast<MAddress>(&slot), zpointer::null, Heap::GetHeap().GetRememberedSet());
            Expect(buffer.LastInstalledStateForTest().oldMark, "prelude_store_buffer_old_obligation");
            buffer.Discard();
        } else {
            ++minorMarkStarts;
            Expect(!old.active, "independent_minor_does_not_start_old");
        }
    };
    // Allocate actual product objects, with an independent export-table root
    // keeping each alive even when a common-root consumer is deliberately cut.
    alignas(TypeInfo) static unsigned char typeStorage[sizeof(TypeInfo)] {};
    auto* type = reinterpret_cast<TypeInfo*>(typeStorage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uint64_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(typeStorage), sizeof(typeStorage));
    std::array<U64, 6> handles {};
    std::array<BaseObject*, 6> witnesses {};
    for (auto& handle : handles) {
        auto* object = MObject::NewObject(type, 16, AllocType::MOVEABLE_OBJECT);
        handle = Heap::GetHeap().RegisterExportRoot(object);
    }
    tracing.testCyclePrepared = [&]() {
        // The real driver has selected and prepared its cycle. Add captures
        // its own state; the test does not provide a phase or generation.
        StoreBarrierBuffer buffer;
        RootSlot slot;
        buffer.Add(reinterpret_cast<MAddress>(&slot), zpointer::null, Heap::GetHeap().GetRememberedSet());
        const auto stored = buffer.LastInstalledStateForTest();
        const bool young = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).active;
        const auto current = resources.GetWorkers(young ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD)
            .GetSnapshot();
        const auto other = resources.GetWorkers(young ? GCCycleGeneration::OLD : GCCycleGeneration::YOUNG)
            .GetSnapshot();
        std::printf("WORKER_PREPARED generation=%s active=%u other_active=%u workers=%u\n",
                    young ? "young" : "old", current.cycleActive, other.cycleActive, current.activeWorkers);
        Expect(current.cycleActive, young ? "worker_young_active_during_cycle" : "worker_old_active_during_cycle");
        Expect(!other.cycleActive, "worker_other_inactive_during_cycle");
        if (young) {
            ++youngLabels;
            Expect(stored.youngMark, "store_buffer_young_label");
        } else {
            ++oldLabels;
            const auto old = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
            Expect(old.sequence == preludeOld.sequence && old.requestIndex == preludeOld.requestIndex,
                   "old_body_keeps_prelude_identity");
            Expect((::g_cjMarkBadMask & MARKED_OLD_MASK) == preludeOldColor,
                   "old_body_keeps_prelude_color");
            for (size_t i = 0; i < handles.size(); ++i) witnesses[i] = Heap::GetHeap().GetExportObject(handles[i]);
            GenerationCycleRootTestAccess::Install(tracing, witnesses);
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
        std::set<BaseObject*> concurrencyRoots;
        RootVisitor concurrentVisitor = [&](ObjectRef& slot) {
            auto* object = to_object(safe(slot.LoadPlain()));
            if (object != nullptr && Heap::IsHeapAddress(object)) concurrencyRoots.insert(object);
        };
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&concurrentVisitor);
        const bool concurrencyIncluded = std::all_of(concurrencyRoots.begin(), concurrencyRoots.end(),
            [&](BaseObject* object) { return observed.count(object) != 0; });
        std::printf("ROOT_CONCURRENCY expected=%zu included=%u\n", concurrencyRoots.size(), concurrencyIncluded);
        Expect(!concurrencyRoots.empty(), "worker_concurrency_witness_exists");
        Expect(concurrencyIncluded, "worker_root_result_contains_concurrency");
        const char* names[] = { "worker_root_result_contains_queued_finalizer",
            "worker_root_result_contains_working_finalizer", "worker_root_result_contains_resurrected",
            "worker_root_result_contains_forward_resurrected", "worker_root_result_contains_cycle_owner",
            "worker_root_result_contains_cycle_external" };
        std::set<BaseObject*> unique(witnesses.begin(), witnesses.end());
        Expect(unique.size() == witnesses.size() && unique.count(nullptr) == 0, "worker_family_witnesses_distinct");
        for (size_t i = 0; i < witnesses.size(); ++i) {
            std::printf("ROOT_FAMILY name=%s object=%p included=%u\n", names[i], witnesses[i],
                        observed.count(witnesses[i]) != 0);
            Expect(observed.count(witnesses[i]) != 0, names[i]);
        }
        GenerationCycleRootTestAccess::Remove(tracing, witnesses);
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
    Expect(SatbBuffer::Young().IsRetiredEmpty(), "major_preserves_completed_young_queue");
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
    Expect(SatbBuffer::Young().IsRetiredEmpty(), "minor_completes_young_queue");
    std::printf("PRODUCT_STATE young_seq=%llu old_seq=%llu young_phase=%u old_phase=%u\n",
        (unsigned long long)y2.sequence, (unsigned long long)o2.sequence,
        (unsigned)y2.phase, (unsigned)o2.phase);
#if defined(MRT_TESTABLE_INTERNALS)
    Expect(youngLabels == 2 && oldLabels == 1, "store_buffer_real_cycle_inputs");
    Expect(rootResults > 0, "worker_root_result_observed");
    Expect(combinedMarkStarts == 1 && minorMarkStarts == 1, "real_mark_start_variants_observed");
    tracing.testYoungMarkStarted = nullptr;
    tracing.testCyclePrepared = nullptr;
    tracing.testRootsResult = nullptr;
    for (auto handle : handles) Heap::GetHeap().RemoveExportObject(handle);
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
