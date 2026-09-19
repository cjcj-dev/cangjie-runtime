// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zForwardingTable.hpp"
#include "root_publication_snapshot.hpp"
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
#include "Heap/z/zRelocate.hpp"

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void PreparePlainRoots(Heap& collector)
    {
        // Match the eager product ordering before the direct TraceHeap fixture.
        // Full driver-entry coverage lives in RawRemapYoungProduct.
        ZRelocate::RemapYoungRoots();
    }
    static void BindNativeRootFixture(Heap& collector, uint32_t workers = 1)
    {
        CHECK(&collector == &Heap::GetHeap());
        ZCollectedHeap::heap()->set_concurrent_gc_threads_for_test(workers);
        for (auto gen : {ZGenerationId::young, ZGenerationId::old}) {
            auto& cycle = Heap::GetHeap().GetZGeneration(gen);
            if (cycle.Snapshot().active) {
                cycle.End();
            }
            if (cycle.Workers() == nullptr) {
                cycle.InitializeWorkers(workers);
            } else {
                cycle.Workers()->set_active_workers(workers);
            }
            cycle.Begin(workers);
        }
        ZGlobalsPointers::initialize();
    }
    static void FlipNativeRootYoung(Heap& collector) { ZGlobalsPointers::flip_young_relocate_start(); }
    static void NativeRootMajorPrelude(Heap& collector)
    {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).End();
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        YoungTypeSetter type(young, ZYoungType::major_partial_roots);
        ZDriver::RunGarbageCollection(1, GC_REASON_YOUNG);
    }
    static void NativeRootTrace(Heap& collector)
    {
        // This fixture enters tracing directly after in-place promotion. Match
        // the old mark-start sequence advance before consuming the new page
        // (ZGC zGeneration.cpp:1212-1237).
        GcUnit::GcHeapFixture::AdvanceGeneration(Generation::Old);
        Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
        Heap::GetHeap().old().Mark().Start();
        auto& old = Heap::GetHeap().old();
        old.concurrent_mark();
        while (!old.pause_mark_end()) old.concurrent_mark_continue();
        old.process_non_strong_references();
    }
    static void RunOldRoots(Heap& collector)
    {
        ZMark::EnumAllCommonRoots(*Heap::GetHeap().GetZGeneration(ZGenerationId::old).Workers());
    }
    static size_t PendingYoungRootWork(Heap& collector)
    {
        return ThreadLocal::GetMarkStacks(*Heap::GetHeap().young().MarkPtr()).Population();
    }
    static void DrainYoungRootWork(Heap& collector)
    {
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *Heap::GetHeap().young().MarkPtr());
        WorkStack work;
        std::vector<BaseObject*> reachable;
        std::unordered_set<MAddress> slots;
        std::unordered_set<MAddress> weak;
        ZMark::TraceYoungClosure(work, false, reachable, slots, weak);
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
void CheckNativeRoot(bool minor, unsigned threadKind = 0)
{
    PrintNativeRootMaps();
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* region = fx.region0;
    region->reset(PageAge::eden);
    region->reset(PageAge::eden);
    ZGeneration::young()->SetTenuringThresholdForTest(1);
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
        const uintptr_t historicalWord = reinterpret_cast<uintptr_t>(from);
        std::memcpy(historicalSlot, &historicalWord, sizeof(historicalWord));
    }
    const uintptr_t before = raw(slot.GetFieldValue());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(region, second));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, { region }));
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTestAccess::FlipNativeRootYoung(collector);
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(region);
    region->MarkForwardingDone();
    auto forwarding = forwarding_for_page(region);
    BaseObject* to = reinterpret_cast<BaseObject*>(forwarding->find(reinterpret_cast<MAddress>(from)));
    std::fprintf(stderr, "NATIVE_ROOT_ORACLE before=%#zx from=%p to=%p slot=%#zx young=%u\n",
                 before, from, to, raw(slot.GetFieldValue()), unsigned(region->IsYoungRegion()));
    GC_EXPECT_TRUE(to != nullptr && to != from);
    if (threadKind != 0) RelocationReceiptTestAccess::PreparePlainRoots(collector);
    if (minor) RelocationReceiptTestAccess::NativeRootMajorPrelude(collector);
    else RelocationReceiptTestAccess::NativeRootTrace(collector);
    const bool currentMarked = region->is_object_strongly_live(from_object(to));
    const bool staleMarked = region->is_object_strongly_live(from_object(from));
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
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::MarkComplete);
    Heap::GetHeap().GetZGeneration(Generation::Young).reset_relocation_set();
    // Observe the product's published old mark stacks after the root task
    // returned and before follow starts (testOldMarkStarted fires at the top
    // of DoTracing, after DoEnumeration). The slot visit is recorded too, so
    // "not enumerated" is distinguishable from "never scanned".
    bool enumerated = false;
    bool visited = false;
    bool observed = false;
    ZMark::testColoredRootResult = [&](ZGenerationId generation, NativeSlot* visitedSlot) {
        visited |= generation == ZGenerationId::old && visitedSlot == &slot;
    };
    ZGeneration::testOldMarkStarted = [&]() {
        observed = true;
        enumerated |= RootPublicationSnapshot::Contains(*Heap::GetHeap().old().MarkPtr(), to);
    };
    RelocationReceiptTestAccess::NativeRootTrace(collector);
    ZGeneration::testOldMarkStarted = nullptr;
    ZMark::testColoredRootResult = nullptr;
    const bool oldMarkedCurrent = region->is_object_strongly_live(from_object(to)) &&
        !region->is_object_strongly_live(from_object(from));
    std::fprintf(stderr, "native_root_after_reset executed=1 observed=%u visited=%u enumerated=%u slot=%#zx expected=%p\n",
                 unsigned(observed), unsigned(visited), unsigned(enumerated), raw(slot.GetFieldValue()), to);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    // Pre-conditions of the target assertion, checked first so a red below
    // cannot be a scan that never ran or an observer that never fired.
    GC_EXPECT_TRUE(observed);
    GC_EXPECT_TRUE(visited);
    // ZMark::mark_object skips an already marked object (zMark.inline.hpp:60-72).
    // Observe actual published stacks, not the retired RootSet input buffer.
    // After young, this is the first old scan and must publish. After old,
    // the already marked current object must not be requeued.
    GC_EXPECT_EQ(enumerated, minor);
    GC_EXPECT_TRUE(to_object(slot.GetTargetObject()) == to);
    GC_EXPECT_TRUE(oldMarkedCurrent);
}

}
GC_OTHER_VM_TEST(ThreadRootCurrent, OrdinaryRootRoutesByTargetGeneration)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::eden);
    fx.region1->reset(PageAge::old);
    RelocationReceiptTestAccess::BindNativeRootFixture(Heap::GetHeap());
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::Mark);
    Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
    Heap::GetHeap().old().Mark().Start();
    Heap::GetHeap().young().Mark().BindWorkers(Heap::GetHeap().young().Workers());
    Heap::GetHeap().young().Mark().Start();
    const size_t oldBefore = Heap::GetHeap().old().Mark().Stripes().Population();
    const size_t youngBefore = Heap::GetHeap().young().Mark().Stripes().Population();
    ZMark::PublishThreadRoot(fx.obj0, false, true);
    ZMark::PublishThreadRoot(fx.obj1, true, true);
    (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), Heap::GetHeap().old().Mark());
    (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), Heap::GetHeap().young().Mark());
    const size_t oldAfter = Heap::GetHeap().old().Mark().Stripes().Population();
    const size_t youngAfter = Heap::GetHeap().young().Mark().Stripes().Population();
    std::fprintf(stderr, "ROOT_TARGET_GEN_ASSERT young=%zu old=%zu\n",
                 youngAfter - youngBefore, oldAfter - oldBefore);
    GC_EXPECT_TRUE(youngAfter > youngBefore);
    GC_EXPECT_TRUE(oldAfter > oldBefore);
}

GC_OTHER_VM_TEST(NativeRootCurrent, MinorPublication) { CheckNativeRoot(true); }
GC_OTHER_VM_TEST(NativeRootCurrent, MajorSeed) { CheckNativeRoot(false); }
GC_OTHER_VM_TEST(P10OldMarkThread, ParkedMutatorStackRootConsumedByWorker)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    BaseObject* held = fx.obj0;

    Mutator* parked = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(parked != nullptr);
    parked->SetManagedContext(false);
    (void)parked->EnterSaferegion(false);
    ObjectRef* root = parked->AddNativeFrameRoot(held);
    GC_EXPECT_TRUE(root != nullptr);
    GC_EXPECT_TRUE(parked->InSaferegion());

    bool workerSawParked = false;
    ZMark::testOldMarkThreadResult = [&](Mutator& mutator) {
        if (&mutator == parked) {
            workerSawParked = true;
        }
    };
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);
    Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
    Heap::GetHeap().old().Mark().Start();
    RelocationReceiptTestAccess::RunOldRoots(collector);
    ZMark::testOldMarkThreadResult = nullptr;

    const bool watermarkDone = parked->GetStackWatermark().IsDone(StackWatermark::epoch_id());
    const bool live = fx.region0->is_object_strongly_live(from_object(held));
    std::fprintf(stderr, "P10_OLD_MARK_THREAD_ASSERT_EXECUTED worker=%u live=%u done=%u epoch=%u\n",
                 unsigned(workerSawParked), unsigned(live), unsigned(watermarkDone), StackWatermark::epoch_id());
    GC_EXPECT_TRUE(workerSawParked);
    GC_EXPECT_TRUE(watermarkDone);

    parked->RemoveNativeFrameRoot(root);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
GC_OTHER_VM_TEST(NativeRootCurrent, ColoredAndNullBoundary)
{
    PrintNativeRootMaps();
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    NativeSlot slot(zpointer::null);
    ZBarrier::WriteStaticRef(slot, fx.obj0);
    GC_EXPECT_TRUE(ZBarrier::ReadStaticRef(slot) == fx.obj0);
    ZBarrier::WriteStaticRef(slot, nullptr);
    GC_EXPECT_TRUE(ZBarrier::ReadStaticRef(slot) == nullptr);
    std::fprintf(stderr, "native_root_boundary executed=1 colored=1 null=1\n");
}

GC_OTHER_VM_TEST(NativeRootCurrent, YoungGoodMarksBeforeHealingAndSkipsRepeat)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    Heap::OnHeapCreated(fx.heapStart);
    Heap::OnHeapExtended(fx.heapStart + GcHeapFixture::kUnits * ZPage::UNIT_SIZE);
    fx.region0->reset(PageAge::eden);
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::Mark);
    Heap::GetHeap().young().Mark().BindWorkers(Heap::GetHeap().young().Workers());
    Heap::GetHeap().young().Mark().Start();
    MarkingStacks::VerifyEmpty(Heap::GetHeap().young().Mark().Stripes().Population());
    // Load-good, but the previous young/old mark epochs: the root must take
    // ZBarrier's mark-young slow path even though no remapping is needed.
    NativeSlot root(to_zpointer(raw(StoreGoodPointer(fx.obj0)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    const size_t before = RelocationReceiptTestAccess::PendingYoungRootWork(collector);
    ZBarrier::MarkYoungGoodBarrierOnOopField(root);
    const bool marked = fx.region0->is_object_strongly_live(from_object(fx.obj0));
    const size_t first = RelocationReceiptTestAccess::PendingYoungRootWork(collector);
    std::fprintf(stderr, "B19_YOUNG_MARK_BEFORE_HEAL executed=1 marked=%u before=%zu after=%zu word=%#lx\n",
                 unsigned(marked), before, first, raw(root.GetFieldValue()));
    GC_EXPECT_TRUE(marked);
    GC_EXPECT_EQ(first, before + 1);
    GC_EXPECT_TRUE(ZPointer::is_marked_young(to_zpointer(raw(root.GetFieldValue()))));
    // The same physical, now young-good slot must not publish another follow.
    ZBarrier::MarkYoungGoodBarrierOnOopField(root);
    GC_EXPECT_EQ(RelocationReceiptTestAccess::PendingYoungRootWork(collector), first);
    RelocationReceiptTestAccess::DrainYoungRootWork(collector);
    GC_EXPECT_EQ(RelocationReceiptTestAccess::PendingYoungRootWork(collector), size_t(0));
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(ThreadRootCurrent, C1StackFieldHistoricalColor) { CheckNativeRoot(false, 1); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C2ObjectRefHistoricalColor) { CheckNativeRoot(false, 2); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C3InvisibleHistoricalColor) { CheckNativeRoot(false, 3); }
GC_OTHER_VM_TEST(ThreadRootCurrent, C4HeaderlessHistoricalColor) { CheckNativeRoot(false, 4); }
#endif

#if defined(MRT_TESTABLE_INTERNALS)
GC_OTHER_VM_TEST(NativeRootCurrent, StrongFinalizerRootPublishesAndMarks)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(PageAge::old);
    GC_EXPECT_FALSE(fixture.region0->is_object_strongly_live(from_object(fixture.obj0)));
    // Seed the real scheduling input through its existing fixture operation.
    // The root task and marker below are the product TraceHeap implementation.
    Heap::GetHeap().GetFinalizerProcessor().EnqueueFinalizableForTest(fixture.obj0);
    bool published = false;
    ZGeneration::testOldMarkStarted = [&]() {
        published |= RootPublicationSnapshot::Contains(*Heap::GetHeap().old().MarkPtr(), fixture.obj0);
    };
    RelocationReceiptTestAccess::NativeRootTrace(collector);
    ZGeneration::testOldMarkStarted = nullptr;
    const bool marked = fixture.region0->is_object_strongly_live(from_object(fixture.obj0));
    std::fprintf(stderr, "ROOT_STORAGE_STRONG_TARGET executed=1 object=%p published=%u marked=%u\n",
                 fixture.obj0, unsigned(published), unsigned(marked));
    // Both observations are read before either target assertion can fail.
    GC_EXPECT_TRUE(published);
    GC_EXPECT_TRUE(marked);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
void CheckRootStorageSegments(unsigned family)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector, 2);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(family != 0 ? PageAge::eden : PageAge::old);
    auto& finalizers = Heap::GetHeap().GetFinalizerProcessor();
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
        ~ResetObserver() { ZMark::testColoredRootResult = nullptr; }
    } reset;
    ZMark::testColoredRootResult = [&](ZGenerationId generation, NativeSlot* slot) {
        if ((generation == ZGenerationId::old) != (family == 0)) { return; }
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
    ZMark::testColoredRootResult = nullptr;
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
    Heap& collector = heap;
    RelocationReceiptTestAccess::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(PageAge::eden);
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
        ~ResetObserver() { ZMark::testColoredRootResult = nullptr; }
    } reset;
    ZMark::testColoredRootResult = [&](ZGenerationId generation, NativeSlot* slot) {
        if (generation != ZGenerationId::young || slot == nullptr) { return; }
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
    ZMark::testColoredRootResult = nullptr;
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
