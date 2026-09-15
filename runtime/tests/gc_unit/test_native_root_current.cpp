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
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void BindNativeRootFixture(CollectorResources& resources, WCollector& collector, RuntimeWorkers& pool, uint32_t workers = 1)
    {
        resources.collectorProxy.currentCollector = &collector;
        resources.runtimeWorkers = &pool;
        resources.gcThreadCount = resources.concurrentGcThreadCount = workers;
        for (auto gen : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
            collector.GetGenerationCycle(gen).InitializeWorkers(workers);
            collector.GetGenerationCycle(gen).Begin(workers);
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
    static size_t PendingYoungRootWork(WCollector& collector)
    {
        return ThreadLocal::GetMarkStacks(*collector.youngMarkDomain).Population();
    }
    static void DrainYoungRootWork(WCollector& collector)
    {
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *collector.youngMarkDomain);
        TracingCollector::WorkStack work;
        std::vector<BaseObject*> reachable;
        WCollector::MinorSlotSet slots;
        WCollector::MinorSlotSet weak;
        collector.TraceYoungClosure(work, false, reachable, slots, weak);
    }
};
}
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
namespace {
void PrintNativeRootMaps()
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libcangjie-runtime.so") != std::string::npos ||
            line.find("libboundscheck.so") != std::string::npos) {
            std::fprintf(stderr, "NATIVE_ROOT_MAPS %s\n", line.c_str());
        }
    }
}

// ZMarkOldRootsTask -> ZMarkOopClosure (zMark.cpp:798-829): colored roots
// resolve before marker publication. CompactRegion supplies the actual to;
// no forwarding mapping or consumer argument is manufactured by this test.
void CheckNativeRoot(bool minor, bool plain = false, unsigned threadKind = 0)
{
    PrintNativeRootMaps();
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
    // Keep a second live object after the root object. In-place compaction
    // reuses the root's old address for that object. A broken remap therefore
    // reaches the address invariant with a valid, but wrong, object instead
    // of being hidden by an earlier object-header check.
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(from) + from->GetSize());
    region->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    NativeSlot slot(StoreGoodPointer(from));
    NativeSlot nullSlot(zpointer::null);
    NativeSlot* roots[] = {&slot, &nullSlot};
    Mutator* thread = nullptr;
    ObjectRef* threadRoot = nullptr;
    RootSlot* historicalSlot = nullptr;
    alignas(16) uintptr_t stackStorage[8] {};
    if (threadKind == 0) {
        heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    } else {
        thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        thread->SetStackTopAddr(reinterpret_cast<uintptr_t>(stackStorage));
        thread->SetStackSize(sizeof(stackStorage));
        if (threadKind == 1) {
            auto* stackObject = reinterpret_cast<BaseObject*>(&stackStorage[2]);
            stackObject->SetClassInfo(fx.typeInfo);
            historicalSlot = &RootSlotAt(static_cast<void*>(&stackStorage[3]));
            threadRoot = thread->AddNativeFrameRoot(stackObject);
        } else if (threadKind == 4) {
            historicalSlot = &RootSlotAt(static_cast<void*>(&stackStorage[2]));
            threadRoot = thread->AddNativeFrameRoot(reinterpret_cast<BaseObject*>(&stackStorage[2]));
        } else if (threadKind == 3) {
            thread->PublishInvisibleRoot(from);
            thread->VisitMutatorRoots([&](ObjectRef& root) {
                if (raw(root.LoadPlain()) == reinterpret_cast<uintptr_t>(from)) historicalSlot = &root;
            });
        } else {
            threadRoot = thread->AddNativeFrameRoot(from);
            historicalSlot = threadRoot;
        }
        const uintptr_t historicalWord = raw(StoreGoodPointer(from));
        std::memcpy(historicalSlot, &historicalWord, sizeof(historicalWord));
    }
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
    (void)bitmap->MarkBits(region->GetAddressOffset(reinterpret_cast<MAddress>(second)), second->GetSize(), region->GetRegionSize());
    RegionList selected("native-root-relocation");
    selected.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
    GC_EXPECT_TRUE(ForwardingTable::BeginForwardingArena(Generation::Young, selected));
    (void)selected.TakeHeadRegion();
    // Invoke the explicit product instantiation, not a header-instantiated
    // fixture copy of the forwarding publication mechanism.
    using Prepare = void (*)(RegionInfo*, MarkView<Generation::Young>);
    void* product = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
    GC_EXPECT_TRUE(product != nullptr);
    auto prepare = reinterpret_cast<Prepare>(dlsym(product,
        "_ZN12MapleRuntime10RegionInfo24PrepareForwardableRegionILNS_10GenerationE0EEEvNS_8MarkViewIXT_EEE"));
    GC_EXPECT_TRUE(prepare != nullptr);
    Dl_info identity{};
    GC_EXPECT_TRUE(dladdr(reinterpret_cast<void*>(prepare), &identity) != 0 &&
                   identity.dli_fname != nullptr && std::strstr(identity.dli_fname, "libcangjie-runtime.so") != nullptr);
    prepare(region, region->GetMarkView<Generation::Young>());
    dlclose(product);
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
    // ZMarkYoungRootsTask -> mark_if_young (zBarrier.inline.hpp:763-767)
    // remaps this promoted root without publishing old strong. The old root
    // task below must independently mark the current address.
    const bool marker = minor ? !currentMarked && !staleMarked : currentMarked && !staleMarked;
    const bool healed = threadKind == 0 ? to_object(slot.GetTargetObject()) == to
        : raw(historicalSlot->LoadPlain()) == reinterpret_cast<uintptr_t>(to);
    if (threadKind != 0) {
        std::fprintf(stderr, "A2_THREAD_ROOT_TARGET kind=C%u from=%p to=%p slot=%#zx current_marked=%u stale_marked=%u healed=%u\n",
                     threadKind, from, to, raw(historicalSlot->LoadPlain()), unsigned(currentMarked),
                     unsigned(staleMarked), unsigned(healed));
    }
    std::fprintf(stderr, "native_root_marker_current executed=1 entry=%s current=%zu stale=%zu expected=%p result=%u\n",
                 minor ? "minor" : "major", size_t(currentMarked), size_t(staleMarked), to, unsigned(marker));
    std::fprintf(stderr, "native_root_healed_current executed=1 before=%#zx after=%#zx expected=%p result=%u\n",
                 before, raw(slot.GetFieldValue()), to, unsigned(healed));
    if (!marker) {
        ::MapleRuntime::GcUnit::Fail(__FILE__, __LINE__, "native_root_marker_current");
    }
    if (!healed) {
        ::MapleRuntime::GcUnit::Fail(__FILE__, __LINE__, "native_root_healed_current");
    }
    if (threadKind != 0) {
        if (threadKind == 3) (void)thread->WithdrawInvisibleRoot();
        else thread->RemoveNativeFrameRoot(threadRoot);
        return;
    }
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
    const bool oldMarkedCurrent = region->IsMarkedObject(region->GetMarkView<Generation::Old>(), to) &&
        !region->IsMarkedObject(region->GetMarkView<Generation::Old>(), from);
    std::fprintf(stderr, "native_root_after_reset executed=1 enumerated=%u slot=%#zx expected=%p\n",
                 unsigned(enumerated), raw(slot.GetFieldValue()), to);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    // ZMark::mark_object skips an already marked object (zMark.inline.hpp:63-75).
    // Observe actual published stacks, not the retired RootSet input buffer.
    // After young, this is the first old scan and must publish. After old,
    // the already marked current object must not be requeued.
    GC_EXPECT_EQ(enumerated, minor);
    GC_EXPECT_TRUE(to_object(slot.GetTargetObject()) == to);
    GC_EXPECT_TRUE(oldMarkedCurrent);
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
    const char* site = minor ? "NativeSlot requires colored value at MarkYoungGoodBarrier"
                             : "NativeSlot requires colored value at ReadStaticRef";
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
// Static literals live outside the GC heap, including ELF .data.rel.ro roots.
// Preserve the pre-existing non-heap path and never attempt to heal read-only
// storage. The heap root is a positive control for actual major enumeration.
GC_OTHER_VM_TEST(NativeRootCurrent, ReadOnlyNonHeapBoundary)
{
    PrintNativeRootMaps();
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    WCollector collector(heap.GetAllocator(), heap.GetCollectorResources());
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(heap.GetCollectorResources(), collector, pool);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    GC_EXPECT_TRUE(page != MAP_FAILED);
    BaseObject* literal = fx.PlaceObject(reinterpret_cast<MAddress>(page) + 128);
    const uintptr_t literalWord = reinterpret_cast<uintptr_t>(literal);
    std::memcpy(page, &literalWord, sizeof(literalWord));
    NativeSlot& literalRoot = NativeSlotAt(page);
    NativeSlot movingRoot(StoreGoodPointer(fx.obj0));
    NativeSlot* roots[] = {&literalRoot, &movingRoot};
    heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    GC_EXPECT_EQ(mprotect(page, pageSize, PROT_READ), 0);
    const bool readCurrent = heap.GetBarrier().ReadStaticRef(literalRoot) == literal;
    bool enumerated = false;
    bool heapPresent = false;
    bool literalPresent = false;
    collector.testRootsResult = [&](GCWorkers::Generation, TracingCollector::RootSet& set) {
        enumerated = true;
        for (auto* node = set.head(); node != nullptr; node = node->next) {
            auto copy = *node;
            while (!copy.empty()) {
                const auto value = copy.back().object();
                heapPresent |= value == fx.obj0;
                literalPresent |= value == literal;
                copy.pop_back();
            }
        }
    };
    RelocationReceiptTestAccess::NativeRootTrace(collector);
    collector.testRootsResult = nullptr;
    const bool unchanged = raw(literalRoot.GetFieldValue()) == literalWord;
    std::fprintf(stderr, "native_root_readonly_boundary executed=1 read=%u enum=%u heap=%u literal=%u unchanged=%u\n",
                 unsigned(readCurrent), unsigned(enumerated), unsigned(heapPresent), unsigned(literalPresent), unsigned(unchanged));
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    munmap(page, pageSize);
    GC_EXPECT_TRUE(readCurrent && enumerated && heapPresent && !literalPresent && unchanged);
}

GC_OTHER_VM_TEST(NativeRootCurrent, ColoredAndNullBoundary)
{
    PrintNativeRootMaps();
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

GC_OTHER_VM_TEST(NativeRootCurrent, YoungGoodMarksBeforeHealingAndSkipsRepeat)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(resources, collector, pool);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    fx.region0->SetYoungRegionFlag(1);
    collector.SetGCPhase(GCCycleGeneration::YOUNG, GC_PHASE_TRACE);
    collector.StartYoungMarkWork();
    // Load-good, but the previous young/old mark epochs: the root must take
    // ZBarrier's mark-young slow path even though no remapping is needed.
    NativeSlot root(to_zpointer(raw(StoreGoodPointer(fx.obj0)) ^ MARKED_YOUNG_MASK ^ MARKED_OLD_MASK));
    const size_t before = RelocationReceiptTestAccess::PendingYoungRootWork(collector);
    heap.GetBarrier().MarkYoungGoodBarrierOnOopField(root);
    const bool marked = fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Young>(), fx.obj0);
    const size_t first = RelocationReceiptTestAccess::PendingYoungRootWork(collector);
    std::fprintf(stderr, "B19_YOUNG_MARK_BEFORE_HEAL executed=1 marked=%u before=%zu after=%zu word=%#lx\n",
                 unsigned(marked), before, first, raw(root.GetFieldValue()));
    GC_EXPECT_TRUE(marked);
    GC_EXPECT_EQ(first, before + 1);
    GC_EXPECT_TRUE(ColourPredicates::is_marked_young(raw(root.GetFieldValue()), ::g_cjMarkBadMask));
    // The same physical, now young-good slot must not publish another follow.
    heap.GetBarrier().MarkYoungGoodBarrierOnOopField(root);
    GC_EXPECT_EQ(RelocationReceiptTestAccess::PendingYoungRootWork(collector), first);
    RelocationReceiptTestAccess::DrainYoungRootWork(collector);
    GC_EXPECT_EQ(RelocationReceiptTestAccess::PendingYoungRootWork(collector), size_t(0));
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(ThreadRootCurrent, C1StackFieldHistoricalColor) { CheckNativeRoot(false, false, 1); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C2ObjectRefHistoricalColor) { CheckNativeRoot(false, false, 2); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C3InvisibleHistoricalColor) { CheckNativeRoot(false, false, 3); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C4HeaderlessHistoricalColor) { CheckNativeRoot(false, false, 4); }
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(NativeRootCurrent, StrongFinalizerRootPublishesAndMarks)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(resources, collector, pool);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    heap.GetRememberedSet().Initialize(fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    fixture.region0->SetYoungRegionFlag(0);
    const auto view = fixture.region0->GetMarkView<Generation::Old>();
    GC_EXPECT_FALSE(fixture.region0->IsMarkedObject(view, fixture.obj0));
    // Seed the real scheduling input through its existing fixture operation.
    // The root task and marker below are the product TraceHeap implementation.
    resources.GetFinalizerProcessor().EnqueueFinalizableForTest(fixture.obj0);
    bool published = false;
    collector.testRootsResult = [&](GCWorkers::Generation, TracingCollector::RootSet& result) {
        for (auto* node = result.head(); node != nullptr; node = node->next) {
            auto copy = *node;
            while (!copy.empty()) {
                published |= copy.back().object() == fixture.obj0;
                copy.pop_back();
            }
        }
    };
    RelocationReceiptTestAccess::NativeRootTrace(collector);
    collector.testRootsResult = nullptr;
    const bool marked = fixture.region0->IsMarkedObject(fixture.region0->GetMarkView<Generation::Old>(), fixture.obj0);
    std::fprintf(stderr, "ROOT_STORAGE_STRONG_TARGET executed=1 object=%p published=%u marked=%u\n",
                 fixture.obj0, unsigned(published), unsigned(marked));
    // Both observations are read before either target assertion can fail.
    GC_EXPECT_TRUE(published && marked);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
void CheckRootStorageSegments(unsigned family)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    RuntimeWorkers pool(2);
    RelocationReceiptTestAccess::BindNativeRootFixture(resources, collector, pool, 2);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    heap.GetRememberedSet().Initialize(fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    fixture.region0->SetYoungRegionFlag(family != 0);
    auto& finalizers = resources.GetFinalizerProcessor();
    // More than two maximum-sized segments: oopStorage.cpp:1101 max_step=10.
    constexpr size_t count = 24 * sizeof(uintptr_t) * CHAR_BIT;
    for (size_t i = 0; i < count; ++i) {
        if (family == 0) { finalizers.EnqueueFinalizableForTest(fixture.obj0); }
        else if (family == 1) { finalizers.RegisterFinalizer(fixture.obj0); }
        else { (void)heap.RegisterExportRoot(fixture.obj0); }
    }
    std::unordered_map<NativeSlot*, size_t> visits;
    const NativeSlotVisitor remember = [&](NativeSlot& slot) { visits.emplace(&slot, 0); };
    if (family == 0) { finalizers.VisitGCRoots(remember); }
    else if (family == 1) { finalizers.VisitFinalizers(remember); }
    else { heap.VisitAllExportRoots(remember); }
    std::mutex mutex;
    std::condition_variable condition;
    std::thread::id paused;
    bool started = false;
    bool otherDone = false;
    size_t otherConsumed = 0;
    bool valuesValid = true;
    struct ResetObserver {
        ~ResetObserver() { TracingCollector::testColoredRootResult = nullptr; }
    } reset;
    collector.testColoredRootResult = [&](GCWorkers::Generation generation, NativeSlot* slot) {
        if ((generation == GCWorkers::Generation::OLD) != (family == 0)) { return; }
        std::unique_lock<std::mutex> lock(mutex);
        if (slot == nullptr) {
            if (!started || std::this_thread::get_id() != paused) {
                otherDone = true;
                condition.notify_all();
            }
            return;
        }
        auto it = visits.find(slot);
        if (it == visits.end()) { return; }
        ++it->second;
        // Read the actual post-closure product slot result, never feed a
        // manufactured intermediate value to a downstream marker.
        valuesValid &= to_object(slot->GetTargetObject()) == fixture.obj0;
        if (!started) {
            started = true;
            paused = std::this_thread::get_id();
            condition.wait(lock, [&] { return otherDone; });
        } else if (std::this_thread::get_id() != paused) {
            ++otherConsumed;
        }
    };
    if (family == 0) { RelocationReceiptTestAccess::NativeRootTrace(collector); }
    else { RelocationReceiptTestAccess::NativeRootMajorPrelude(collector); }
    collector.testColoredRootResult = nullptr;
    bool exactlyOnce = visits.size() == count;
    for (const auto& entry : visits) { exactlyOnce &= entry.second == 1; }
    std::fprintf(stderr,
        "ROOT_SEGMENT_TARGET executed=1 family=%u slots=%zu other_consumed=%zu exactly_once=%u values_valid=%u\n",
        family, visits.size(), otherConsumed, unsigned(exactlyOnce), unsigned(valuesValid));
    // Every target fact is observed before any assertion can mask another.
    GC_EXPECT_TRUE(otherConsumed > 0 && exactlyOnce && valuesValid);
}
}
GC_OTHER_VM_TEST(RootStorageSegments, Strong) { CheckRootStorageSegments(0); }
GC_OTHER_VM_TEST(RootStorageSegments, WeakFinalizer) { CheckRootStorageSegments(1); }
GC_OTHER_VM_TEST(RootStorageSegments, Export) { CheckRootStorageSegments(2); }
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(RootStorageLifetime, ReleaseAndGrowDuringYoungTask)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    auto& resources = heap.GetCollectorResources();
    WCollector collector(heap.GetAllocator(), resources);
    RuntimeWorkers pool(1);
    RelocationReceiptTestAccess::BindNativeRootFixture(resources, collector, pool);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    heap.GetRememberedSet().Initialize(fixture.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    fixture.region0->SetYoungRegionFlag(1);
    std::vector<U64> original;
    for (size_t i = 0; i < sizeof(uintptr_t) * CHAR_BIT; ++i) {
        original.push_back(heap.RegisterExportRoot(fixture.obj0));
    }
    auto& storage = heap.GetExportRootStorage();
    const size_t before = storage.BlockCountForTest();
    size_t pinned = 0;
    size_t during = 0;
    size_t grown = 0;
    bool observed = false;
    U64 added = 0;
    NativeSlot* addedSlot = nullptr;
    bool newSlotVisited = false;
    struct ResetObserver {
        ~ResetObserver() { TracingCollector::testColoredRootResult = nullptr; }
    } reset;
    collector.testColoredRootResult = [&](GCWorkers::Generation generation, NativeSlot* slot) {
        if (generation != GCWorkers::Generation::YOUNG || slot == nullptr) { return; }
        if (slot == addedSlot) { newSlotVisited = true; }
        if (observed) { return; }
        observed = true;
        pinned = storage.ConcurrentIterationsForTest();
        // Existing block is full, so growth must publish a new active array.
        added = heap.RegisterExportRoot(fixture.obj0);
        heap.VisitAllExportRoots([&](NativeSlot& root) {
            if (&root != slot) { addedSlot = &root; }
        });
        // The added slot is last in the new block. Nested iteration above has
        // finished before the release, leaving the root task's state pinned.
        grown = storage.BlockCountForTest();
        for (U64 handle : original) { heap.RemoveExportObject(handle); }
        during = storage.BlockCountForTest();
    };
    RelocationReceiptTestAccess::NativeRootMajorPrelude(collector);
    collector.testColoredRootResult = nullptr;
    const size_t after = storage.BlockCountForTest();
    const size_t remaining = storage.AllocationCount();
    std::fprintf(stderr,
        "ROOT_LIFETIME_TARGET executed=1 observed=%u before=%zu pinned=%zu grown=%zu during=%zu after=%zu remaining=%zu new_visited=%u\n",
        unsigned(observed), before, pinned, grown, during, after, remaining, unsigned(newSlotVisited));
    heap.RemoveExportObject(added);
    GC_EXPECT_TRUE(observed && pinned > 0 && grown == before + 1 && during == grown &&
                   after == grown - 1 && remaining == 1 && !newSlotVisited);
}
#endif
