// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "ObjectModel/RefField.inline.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void BindNativeRootFixture(CollectorResources& resources, WCollector& collector, RuntimeWorkers& pool)
    {
        resources.collectorProxy.currentCollector = &collector;
        resources.runtimeWorkers = &pool;
        resources.gcThreadCount = resources.concurrentGcThreadCount = 1;
        for (auto gen : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
            collector.GetGenerationCycle(gen).InitializeWorkers(1);
            collector.GetGenerationCycle(gen).Begin(1);
        }
        collector.set_good_masks();
    }
    static void FlipNativeRootYoung(WCollector& collector) { collector.flip_young_relocate_start(); }
    static void NativeRootMajorPrelude(WCollector& collector)
    {
        collector.GetGenerationCycle(GCCycleGeneration::OLD).End();
        auto& young = collector.GetGenerationCycle(GCCycleGeneration::YOUNG);
        YoungTypeSetter type(young, ZYoungType::major_partial_roots);
        collector.RunGarbageCollection(1, GC_REASON_YOUNG);
    }
    static void NativeRootTrace(WCollector& collector)
    {
        collector.StartOldMarkWork();
        collector.TraceHeap();
    }
};
}
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
// ZMarkOldRootsTask -> ZMarkOopClosure (zMark.cpp:798-829): colored roots
// resolve before marker publication. CompactRegion supplies the actual to;
// no forwarding mapping or consumer argument is manufactured by this test.
void CheckNativeRoot(bool minor, bool plain = false)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(resources, collector, pool);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    heap.GetRememberedSet().Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    RegionInfo* region = fx.region0;
    region->SetYoungRegionFlag(1);
    region->SetYoungAge(1);
    resources.GetGCStats(GCCycleGeneration::YOUNG).tenuringThreshold = 1;
    BaseObject* dead = fx.PlaceObject(region->GetRegionStart());
    BaseObject* from = fx.PlaceObject(region->GetRegionStart() + dead->GetSize());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(from) + from->GetSize());
    NativeSlot slot(StoreGoodPointer(from));
    NativeSlot nullSlot(zpointer::null);
    NativeSlot* roots[] = {&slot, &nullSlot};
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    if (plain) {
        // Reproduce the retired compiler static-store fast path at its carrier,
        // not in the consumer or a collector substitute.
        const uintptr_t word = reinterpret_cast<uintptr_t>(from);
        std::memcpy(&slot, &word, sizeof(word));
    }
    const uintptr_t before = raw(slot.GetFieldValue());
    LiveInfo* live = fx.PlantLiveInfo(region);
    auto* bitmap = fx.PlantMarkBitmap<Generation::Young>(live, region->GetRegionSize());
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(from)), from->GetSize(), region->GetRegionSize());
    RegionList selected("native-root-relocation");
    selected.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Young, selected));
    (void)selected.TakeHeadRegion();
    region->PrepareForwardableRegion(region->GetMarkView<Generation::Young>());
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GC_PHASE_PREFORWARD);
    RelocationReceiptTestAccess::FlipNativeRootYoung(collector);
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(region);
    region->MarkForwardingDone();
    auto forwarding = ForwardingTable::RetainPageOwner(region);
    BaseObject* to = reinterpret_cast<BaseObject*>(forwarding->find(reinterpret_cast<MAddress>(from)));
    std::fprintf(stderr, "NATIVE_ROOT_ORACLE before=%#zx from=%p to=%p slot=%#zx young=%u\n",
                 before, from, to, raw(slot.GetFieldValue()), unsigned(region->IsYoungRegion()));
    GC_EXPECT_TRUE(to != nullptr && to != from);
    if (minor) RelocationReceiptTestAccess::NativeRootMajorPrelude(collector);
    else RelocationReceiptTestAccess::NativeRootTrace(collector);
    const bool currentMarked = region->IsMarkedObject(region->GetMarkView<Generation::Old>(), to);
    const bool staleMarked = region->IsMarkedObject(region->GetMarkView<Generation::Old>(), from);
    const bool marker = currentMarked && !staleMarked;
    const bool healed = to_object(slot.GetTargetObject()) == to;
    std::fprintf(stderr, "native_root_marker_current executed=1 entry=%s current=%zu stale=%zu expected=%p result=%u\n",
                 minor ? "minor" : "major", size_t(currentMarked), size_t(staleMarked), to, unsigned(marker));
    std::fprintf(stderr, "native_root_healed_current executed=1 before=%#zx after=%#zx expected=%p result=%u\n",
                 before, raw(slot.GetFieldValue()), to, unsigned(healed));
    GC_EXPECT_TRUE(marker);
    GC_EXPECT_TRUE(healed);
    GC_EXPECT_TRUE(to_object(nullSlot.GetTargetObject()) == nullptr);
    collector.SetGCPhase(GCCycleGeneration::OLD, GC_PHASE_MARK_COMPLETE);
    ForwardingTable::ResetRelocationSet(Generation::Young);
    bool enumerated = false;
    collector.testRootsResult = [&](GCWorkers::Generation, TracingCollector::RootSet& set) {
        for (auto* node = set.head(); node != nullptr; node = node->next) {
            auto copy = *node;
            while (!copy.empty()) {
                enumerated |= copy.back().object() == to;
                copy.pop_back();
            }
        }
    };
    RelocationReceiptTestAccess::NativeRootTrace(collector);
    collector.testRootsResult = nullptr;
    std::fprintf(stderr, "native_root_after_reset executed=1 enumerated=%u slot=%#zx expected=%p\n",
                 unsigned(enumerated), raw(slot.GetFieldValue()), to);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    GC_EXPECT_TRUE(enumerated && to_object(slot.GetTargetObject()) == to);
}
void CheckPlainRejected(bool minor)
{
    int pipefd[2];
    GC_EXPECT_EQ(pipe(pipefd), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        CheckNativeRoot(minor, true);
        _exit(0);
    }
    close(pipefd[1]);
    std::string output;
    char buffer[4096];
    ssize_t size;
    while ((size = read(pipefd[0], buffer, sizeof(buffer))) > 0) output.append(buffer, size);
    close(pipefd[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const char* site = minor ? "NativeSlot requires colored value at ReadStaticRef"
                             : "NativeSlot requires colored value at EnumRefFieldRoot";
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
                          output.find(site) != std::string::npos;
    std::fprintf(stderr, "%snative_root_encoding_rejected executed=1 entry=%s status=%d result=%u\n",
                 output.c_str(), minor ? "minor" : "major", status, unsigned(rejected));
    GC_EXPECT_TRUE(rejected);
}

}
GC_OTHER_VM_TEST(NativeRootCurrent, MinorPublication) { CheckNativeRoot(true); }
GC_OTHER_VM_TEST(NativeRootCurrent, MajorSeed) { CheckNativeRoot(false); }
GC_OTHER_VM_TEST(NativeRootCurrent, PlainMinorRejected) { CheckPlainRejected(true); }
GC_OTHER_VM_TEST(NativeRootCurrent, PlainMajorRejected) { CheckPlainRejected(false); }
GC_OTHER_VM_TEST(NativeRootCurrent, ColoredAndNullBoundary)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    WCollector collector(heap.GetAllocator(), heap.GetCollectorResources());
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(heap.GetCollectorResources(), collector, pool);
    NativeSlot slot(zpointer::null);
    heap.GetBarrier().WriteStaticRef(slot, fx.obj0);
    GC_EXPECT_TRUE(heap.GetBarrier().ReadStaticRef(slot) == fx.obj0);
    heap.GetBarrier().WriteStaticRef(slot, nullptr);
    GC_EXPECT_TRUE(heap.GetBarrier().ReadStaticRef(slot) == nullptr);
    std::fprintf(stderr, "native_root_boundary executed=1 colored=1 null=1\n");
}
#endif
