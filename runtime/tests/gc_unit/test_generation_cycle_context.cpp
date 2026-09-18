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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "root_publication_snapshot.hpp"

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
// Seed inputs of the real collector; never call or reconstruct the enum task
// in the test. The real major request owns execution and merging.
// This fixture checks root scanning, not finalizer scheduling or invocation.
//
// Strong native roots are the entries of the strong OopStorage
// (zRootsIterator.cpp:159-162 ZRootsIteratorStrongColored::apply ->
// :102-105 ZOopStorageSetIteratorStrong::apply ->
// oopStorageSetParState.inline.hpp:38-42 OopStorageSetStrongParState::oops_do;
// ours OopStorageSetIteratorStrong over
// FinalizerProcessor::StrongRootStorage()). The storage is the root truth; the
// processor's finalizables/workingFinalizables lists own no slots and are its
// private scheduling state, guarded by the predicate CHECK in
// FinalizerProcessor::HasFinalizableJob(). Seed the storage through its own
// public Allocate/Release (gtest test_oopStorage.cpp shape) and leave the
// scheduling lists untouched, so the finalizer thread never sees a queue
// whose predicate it did not set.
struct ZGenerationRootTestAccess {
    inline static std::array<NativeSlot*, 2> strongSlots {};
    static void Install(HeapGcState& collector, const std::array<BaseObject*, 6>& objects)
    {
        OopStorage& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
        for (size_t i = 0; i < strongSlots.size(); ++i) {
            NativeSlot coloured(zpointer::null);
            ZBarrier::WriteStaticRef(coloured, objects[i]);
            strongSlots[i] = storage.Allocate();
            strongSlots[i]->StoreColoured(coloured.GetFieldValue(), std::memory_order_relaxed);
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
    static void Remove(HeapGcState& collector, const std::array<BaseObject*, 6>& objects)
    {
        OopStorage& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
        for (NativeSlot*& slot : strongSlots) {
            if (slot == nullptr) continue;
            storage.Release(slot);
            slot = nullptr;
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
    HeapGcState& collector = Heap::GetHeap().GetCollector();
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
    ZWorkers& youngWorkers = *Heap::GetHeap().young().Workers();
    ZWorkers& oldWorkers = *Heap::GetHeap().old().Workers();
    // ZWorkers (zWorkers.cpp:60-64) initializes each generation with all
    // concurrent workers active, and no generation is active before a cycle.
    Expect(youngWorkers.active_workers() == concurrent && oldWorkers.active_workers() == concurrent,
           "worker_startup_active_budget");
    Expect(!youngWorkers.is_active() && !oldWorkers.is_active(), "worker_startup_inactive");
    std::printf("WORKER_INPUT cpu=%zu heap=%zu region=%zu concurrent=%zu\n",
                cpuCount, heapBytes, regionBytes, concurrent);
#if defined(MRT_TESTABLE_INTERNALS)
    auto& tracing = static_cast<HeapGcState&>(collector);
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
        const auto young = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
        const auto old = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
        Expect(young.active, "young_mark_start_active");
        if (Heap::GetHeap().GetZGeneration(ZGenerationId::young).IsMajorRoots()) {
            ++combinedMarkStarts;
            preludeOld = old;
            preludeOldColor = ::g_cjMarkBadMask & ZPointerMarkedOldMask;
            Expect(old.active && old.phase == ZGenerationPhase::Mark && old.reason == GC_REASON_USER,
                   "prelude_starts_old_mark");
            StoreBarrierBuffer buffer;
            RootSlot slot;
            buffer.add(reinterpret_cast<MAddress>(&slot), zpointer::null);
            Expect(buffer.Pending() == 1u, "prelude_store_buffer_old_obligation");
            buffer.Flush();
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
        buffer.add(reinterpret_cast<MAddress>(&slot), zpointer::null);
        const auto storedPending = buffer.Pending();
        const bool young = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young).active;
        ZWorkers& current = *Heap::GetHeap().GetZGeneration(
            young ? ZGenerationId::young : ZGenerationId::old).Workers();
        ZWorkers& other = *Heap::GetHeap().GetZGeneration(
            young ? ZGenerationId::old : ZGenerationId::young).Workers();
        std::printf("WORKER_PREPARED generation=%s active=%u other_active=%u workers=%u\n",
                    young ? "young" : "old", current.is_active(), other.is_active(), current.active_workers());
        Expect(current.active_workers() == concurrent, "worker_prepared_concurrent_budget");
        Expect(current.is_active(), young ? "worker_young_active_during_cycle" : "worker_old_active_during_cycle");
        Expect(!other.is_active(), "worker_other_inactive_during_cycle");
        if (young) {
            ++youngLabels;
            Expect(storedPending == 1u, "store_buffer_young_color");
        } else {
            ++oldLabels;
            const auto old = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
            Expect(old.sequence == preludeOld.sequence && old.requestIndex == preludeOld.requestIndex,
                   "old_body_keeps_prelude_identity");
            Expect((::g_cjMarkBadMask & ZPointerMarkedOldMask) == preludeOldColor,
                   "old_body_keeps_prelude_color");
            for (size_t i = 0; i < handles.size(); ++i) witnesses[i] = Heap::GetHeap().GetExportObject(handles[i]);
            ZGenerationRootTestAccess::Install(tracing, witnesses);
            Expect(storedPending == 1u, "store_buffer_old_color");
        }
        buffer.Flush();
    };
    // Old root publication, read from the product's own published mark stacks
    // at the top of DoTracing (zGeneration.cpp: after DoEnumeration returned,
    // before TracingImpl pops anything). Export roots are not in this window:
    // EnumAllExportRoots fills the driver-local foreign stack that only
    // ProcessExportRoots consumes, so a witness is observed here only if its
    // family scan published it.
    tracing.testOldMarkStarted = [&]() {
        const std::set<BaseObject*> observed = RootPublicationSnapshot::Objects(*tracing.MajorMark());
        size_t expected = 0;
        bool included = true;
        Heap::GetHeap().VisitStaticRoots([&](NativeSlot& slot) {
            auto* object = to_object(slot.GetTargetObject());
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
        const char* names[] = { "worker_root_result_contains_strong_storage_slot0",
            "worker_root_result_contains_strong_storage_slot1", "worker_root_result_contains_resurrected",
            "worker_root_result_contains_forward_resurrected", "worker_root_result_contains_cycle_owner",
            "worker_root_result_contains_cycle_external" };
        std::set<BaseObject*> unique(witnesses.begin(), witnesses.end());
        Expect(unique.size() == witnesses.size() && unique.count(nullptr) == 0, "worker_family_witnesses_distinct");
        for (size_t i = 0; i < witnesses.size(); ++i) {
            std::printf("ROOT_FAMILY name=%s object=%p included=%u\n", names[i], witnesses[i],
                        observed.count(witnesses[i]) != 0);
            Expect(observed.count(witnesses[i]) != 0, names[i]);
        }
        ZGenerationRootTestAccess::Remove(tracing, witnesses);
        ++rootResults;
        const auto old = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
        std::printf("ROOT_RESULT expected_static=%zu observed_objects=%zu old_active=%u old_phase=%u\n",
                    expected, observed.size(), unsigned(old.active), static_cast<unsigned>(old.phase));
        Expect(expected > 0, "worker_root_witness_exists");
        Expect(included, "worker_root_result_contains_statics");
        Expect(old.active && old.phase == ZGenerationPhase::Mark, "worker_root_result_owner");
    };
#endif
    auto y0 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
    auto o0 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    Expect(youngWorkers.active_workers() == concurrent && oldWorkers.active_workers() == concurrent,
           "worker_major_phase_budget");
    Expect(!youngWorkers.is_active() && !oldWorkers.is_active(), "worker_major_completion");
    // ZStatCycle::at_end (zStat.cpp:1252-1253) reset the old generation's
    // worker accounting at the end of the major; a minor must not add to it.
    const auto oldWorkerStats1 = Heap::GetHeap().GetZGeneration(ZGenerationId::old).StatWorkers()->stats();
    auto y1 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
    auto o1 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
    Expect(y1.sequence == y0.sequence + 1, "major_prelude_young_sequence");
    Expect(o1.sequence == o0.sequence + 1, "major_old_sequence");
    Expect(o1.reason == GC_REASON_USER && !o1.active, "major_reason_completion");
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    Expect(youngWorkers.active_workers() == concurrent && !youngWorkers.is_active(), "worker_minor_phase_budget");
    const auto oldWorkerStats2 = Heap::GetHeap().GetZGeneration(ZGenerationId::old).StatWorkers()->stats();
    Expect(oldWorkerStats2._accumulated_duration == oldWorkerStats1._accumulated_duration &&
           oldWorkerStats2._accumulated_time == oldWorkerStats1._accumulated_time &&
           oldWorkers.active_workers() == concurrent && !oldWorkers.is_active(), "worker_minor_preserves_old");
    auto y2 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
    auto o2 = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::old);
    Expect(y2.sequence == y1.sequence + 1, "minor_sequence");
    Expect(Same(o1, o2), "minor_preserves_old_state");
    Expect(y2.reason == GC_REASON_YOUNG && !y2.active, "minor_reason_completion");
    Expect(!y2.active, "minor_phase_consumer");
    std::printf("PRODUCT_STATE young_seq=%llu old_seq=%llu young_phase=%u old_phase=%u\n",
        (unsigned long long)y2.sequence, (unsigned long long)o2.sequence,
        static_cast<unsigned>(y2.phase), static_cast<unsigned>(o2.phase));
#if defined(MRT_TESTABLE_INTERNALS)
    Expect(youngLabels == 2 && oldLabels == 1, "store_buffer_real_cycle_inputs");
    Expect(rootResults > 0, "worker_root_result_observed");
    Expect(combinedMarkStarts == 1 && minorMarkStarts == 1, "real_mark_start_variants_observed");
    tracing.testYoungMarkStarted = nullptr;
    tracing.testCyclePrepared = nullptr;
    tracing.testOldMarkStarted = nullptr;
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
