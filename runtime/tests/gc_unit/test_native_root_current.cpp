// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_generation_test.hpp"
#include "b09_runtime_fixture.hpp"
#include "mark_publication_fixture.hpp"
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
#include "gc_generation_test.hpp"
#include "gc_product_access_test.hpp"
#include "finalizer_processor_test.hpp"

namespace MapleRuntime {
class RelocationReceiptTest {
public:
    static void PreparePlainRoots(Heap& collector)
    {
        // Match the eager product ordering before the direct TraceHeap fixture.
        // Full driver-entry coverage lives in RawRemapYoungProduct.
        ZRelocate::RemapYoungRoots();
    }
    static void BindNativeRootFixture(Heap& collector, uint32_t workers = 1)
    {
        CHECK(&collector == &Heap::GetHeap());
        ZCollectedHeapTest::SetWorkers(workers);
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
        // ZGC zGeneration.cpp:1212-1237: use the product mark-start to
        // establish colors and sequence before the real concurrent root task.
        auto& old = Heap::GetHeap().old();
        old.End();
        old.mark_start();
        old.concurrent_mark();
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
    RelocationReceiptTest::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* region = fx.region0;
    region->reset(PageAge::eden);
    region->reset(PageAge::eden);
    ZGenerationTest::SetTenuringThreshold(*ZGeneration::young(), 1);
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
    size_t frameMark = 0;
    RootSlot* historicalSlot = nullptr;
    alignas(16) uintptr_t stackStorage[8] {};
    if (threadKind == 0) {
        heap.RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    } else {
        thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        thread->SetManagedContext(false);
        frameMark = thread->NativeFrameRootCount();
        (void)thread->EnterSaferegion(false);
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
    RelocationReceiptTest::FlipNativeRootYoung(collector);
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(region);
    region->MarkForwardingDone();
    auto forwarding = forwarding_for_page(region);
    BaseObject* to = reinterpret_cast<BaseObject*>(forwarding->find(reinterpret_cast<MAddress>(from)));
    // CompactRegion does not promote the page. Use the product promotion
    // operation so the old root task is measured against an actual old page.
    ZPage* promoted = region->clone_for_promotion();
    heap.young().flip_promote(region, promoted);
    fx.region0 = promoted;
    region = promoted;
    std::fprintf(stderr, "NATIVE_ROOT_ORACLE before=%#zx from=%p to=%p slot=%#zx young=%u\n",
                 before, from, to, raw(slot.GetFieldValue()), unsigned(region->IsYoungRegion()));
    GC_EXPECT_TRUE(to != nullptr && to != from);
    if (minor) RelocationReceiptTest::NativeRootMajorPrelude(collector);
    else RelocationReceiptTest::NativeRootTrace(collector);
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
        else thread->PopNativeFrameRootsTo(frameMark);
        return;
    }
    GC_EXPECT_TRUE(to_object(nullSlot.GetTargetObject()) == nullptr);
    Heap::GetHeap().GetZGeneration(Generation::Young).reset_relocation_set();
    // The preceding major prelude already started this old mark cycle.
    // Continue its root task without a second mark-color flip.
    heap.old().concurrent_mark();
    const bool oldMarkedCurrent = region->is_object_strongly_live(from_object(to)) &&
        !region->is_object_strongly_live(from_object(from));
    std::fprintf(stderr, "native_root_after_reset executed=1 marked=%u slot=%#zx expected=%p\n",
                 unsigned(oldMarkedCurrent), raw(slot.GetFieldValue()), to);
    heap.UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 2);
    // The product's final mark and healed root are the result; receipt events are gone.
    GC_EXPECT_TRUE(to_object(slot.GetTargetObject()) == to);
    GC_EXPECT_TRUE(oldMarkedCurrent);
}

}
namespace {
void CheckSavedRootColor(bool invisible, bool watermark = true, bool twoRounds = false)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0;
    page->reset(PageAge::eden);
    ZGenerationTest::SetTenuringThreshold(heap.young(), 1);
    BaseObject* dead = fx.PlaceObject(page->GetRegionStart());
    BaseObject* earlier = fx.PlaceObject(page->GetRegionStart() + dead->GetSize());
    BaseObject* from = fx.PlaceObject(reinterpret_cast<MAddress>(earlier) + earlier->GetSize());
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(from) + from->GetSize());
    page->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    Mutator* thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    thread->SetManagedContext(false);
    (void)thread->EnterSaferegion(false);
    const size_t roots = thread->NativeFrameRootCount();
    RootSlot* slot;
    if (invisible) {
        thread->PublishInvisibleRoot(from);
        slot = reinterpret_cast<RootSlot*>(thread->GetGCData().invisibleRoot);
    } else {
        slot = thread->AddNativeFrameRoot(from);
    }
    if (twoRounds) {
        ZGlobalsPointers::flip_old_relocate_start();
        const bool first = thread->GcPhaseEnum(false, watermark ? StackWatermark::epoch_id() : 0);
        GC_EXPECT_TRUE(first);
        GC_EXPECT_EQ(raw(slot->LoadPlain()), reinterpret_cast<uintptr_t>(from));
    }
    const uintptr_t savedColor = thread->GetGCData().loadGoodMask;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, earlier));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, second));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {page}));
    heap.young().set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::FlipNativeRootYoung(heap);
    // Compact via the product implementation. Keep another live object at the
    // old address so a color cut reaches the address assertion, not an invalid
    // header. The earlier live object also makes a duplicate remap return a
    // valid but wrong object, exposing double consumption at that assertion.
    // Neither the forwarding value nor the root result is fabricated.
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(page);
    page->MarkForwardingDone();
    const MAddress expected = forwarding_for_page(page)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    const bool scanned = thread->GcPhaseEnum(false, watermark ? StackWatermark::epoch_id() : 0);
    const uintptr_t observed = raw(slot->LoadPlain());
    std::fprintf(stderr,
        "SAVED_ROOT_COLOR_TARGET invisible=%u scanned=%u saved=%#lx current=%#lx from=%p observed=%#lx expected=%#lx\n",
        unsigned(invisible), unsigned(scanned), savedColor, ZPointerLoadGoodMask, from, observed, expected);
    // Evaluate the product result before cleanup can consume it again.
    GC_EXPECT_EQ(observed, expected);
    GC_EXPECT_TRUE(scanned);
    if (invisible) { thread->WithdrawInvisibleRoot(); }
    thread->PopNativeFrameRootsTo(roots);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
}
GC_OTHER_VM_TEST(ThreadRootCurrent, TwoEpochNativeFrameRoot) { CheckSavedRootColor(false, true, true); }
GC_OTHER_VM_TEST(ThreadRootCurrent, TwoEpochInvisibleRoot) { CheckSavedRootColor(true, true, true); }
GC_OTHER_VM_TEST(ThreadRootCurrent, SavedColorNativeFrameRoot) { CheckSavedRootColor(false); }
GC_OTHER_VM_TEST(ThreadRootCurrent, SavedColorInvisibleRoot) { CheckSavedRootColor(true); }
GC_OTHER_VM_TEST(ThreadRootCurrent, SavedColorDirectNativeFrameRoot) { CheckSavedRootColor(false, false); }
GC_OTHER_VM_TEST(ThreadRootCurrent, SavedColorDirectInvisibleRoot) { CheckSavedRootColor(true, false); }

GC_OTHER_VM_TEST(ThreadRootCurrent, RemapYoungRootsNativeFrameRoot)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0;
    page->reset(PageAge::eden);
    ZGenerationTest::SetTenuringThreshold(heap.young(), 1);
    BaseObject* dead = fx.PlaceObject(page->GetRegionStart());
    BaseObject* earlier = fx.PlaceObject(page->GetRegionStart() + dead->GetSize());
    BaseObject* from = fx.PlaceObject(reinterpret_cast<MAddress>(earlier) + earlier->GetSize());
    BaseObject* second = fx.PlaceObject(reinterpret_cast<MAddress>(from) + from->GetSize());
    page->SetRegionAllocPtr(reinterpret_cast<MAddress>(second) + second->GetSize());
    Mutator* thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    thread->SetManagedContext(false);
    (void)thread->EnterSaferegion(false);
    const size_t roots = thread->NativeFrameRootCount();
    RootSlot* slot = thread->AddNativeFrameRoot(from);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, earlier));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, second));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {page}));
    heap.young().set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::FlipNativeRootYoung(heap);
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    manager.CompactRegion(page);
    page->MarkForwardingDone();
    const MAddress expected = forwarding_for_page(page)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    ZRelocate::RemapYoungRoots();
    const uintptr_t observed = raw(slot->LoadPlain());
    std::fprintf(stderr, "REMAP_YOUNG_ROOTS_THREAD from=%p observed=%#lx expected=%#lx\n", from, observed, expected);
    GC_EXPECT_EQ(observed, expected);
    thread->PopNativeFrameRootsTo(roots);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}

GC_OTHER_VM_TEST(ThreadRootCurrent, YoungRelocateSkipsForeignIncompleteFrom)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    fx.region1->reset(PageAge::old);
    BaseObject* held = fx.PlaceObject(fx.region1->GetRegionStart());
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(held) + held->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1, held));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {fx.region1}));
    GC_EXPECT_TRUE(!fx.region1->IsForwardingDone());
    GC_EXPECT_TRUE(forwarding_for_page(fx.region1) != nullptr);
    heap.young().set_phase(ZGenerationPhase::Relocate);
    const std::vector<BaseObject*> none;
    const std::unordered_set<MAddress> emptySlots;
    const std::unordered_map<MAddress, BaseObject*> emptyBases;
    heap.young().EvacuateYoungRegions(none, emptySlots, false, emptyBases, nullptr);
    GC_EXPECT_TRUE(forwarding_for_page(fx.region1) != nullptr);
    GC_EXPECT_TRUE(!fx.region1->IsForwardingDone());
}

GC_OTHER_VM_TEST(ThreadRootCurrent, OrdinaryRootRoutesByTargetGeneration)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    fx.region0->reset(PageAge::eden);
    fx.region1->reset(PageAge::old);
    MarkPublicationFixture marking;
    size_t young = 0;
    size_t old = 0;
    // Real native-frame producers feed the product root phase. The old scan
    // context must not select the domain for either current target.
    Mutator* thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    thread->SetManagedContext(false);
    (void)thread->EnterSaferegion(false);
    const size_t rootMark = thread->NativeFrameRootCount();
    (void)thread->AddNativeFrameRoot(fx.obj0);
    (void)thread->AddNativeFrameRoot(fx.obj1);
    const bool scanned = thread->GcPhaseEnum(false);
    marking.DrainDomain(*Heap::GetHeap().young().MarkPtr(), [&](BaseObject* object, bool follow) {
        GC_EXPECT_TRUE(object == fx.obj0);
        GC_EXPECT_TRUE(follow);
        ++young;
    });
    marking.DrainOld([&](BaseObject* object, bool follow) {
        GC_EXPECT_TRUE(object == fx.obj1);
        GC_EXPECT_TRUE(follow);
        ++old;
    });
    bool youngCurrent = false;
    bool oldCurrent = false;
    thread->VisitMutatorRoots([&](ObjectRef& root) {
        youngCurrent |= to_object(safe(root.LoadPlain())) == fx.obj0;
        oldCurrent |= to_object(safe(root.LoadPlain())) == fx.obj1;
    });
    const bool slotsCurrent = youngCurrent && oldCurrent;
    thread->PopNativeFrameRootsTo(rootMark);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    std::fprintf(stderr, "ROOT_TARGET_GEN_ASSERT executed=1 scanned=%u current=%u young=%zu old=%zu\n",
                 unsigned(scanned), unsigned(slotsCurrent), young, old);
    GC_EXPECT_TRUE(scanned && slotsCurrent);
    GC_EXPECT_EQ(young, 1u);
    GC_EXPECT_EQ(old, 1u);
}

GC_OTHER_VM_TEST(NativeRootCurrent, MinorPublication) { CheckNativeRoot(true); }
GC_OTHER_VM_TEST(NativeRootCurrent, MajorSeed) { CheckNativeRoot(false); }
GC_OTHER_VM_TEST(P10OldMarkThread, ParkedMutatorStackRootConsumedByWorker)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTest::BindNativeRootFixture(collector);
    BaseObject* held = fx.obj0;

    Mutator* parked = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(parked != nullptr);
    parked->SetManagedContext(false);
    (void)parked->EnterSaferegion(false);
    const size_t frameMark = parked->NativeFrameRootCount();
    ObjectRef* root = parked->AddNativeFrameRoot(held);
    GC_EXPECT_TRUE(root != nullptr);
    GC_EXPECT_TRUE(parked->InSaferegion());

    heap.old().End();
    // zGeneration.cpp:1212-1237, zMark.cpp:797-834: mark-start establishes
    // the color/sequence before workers consume roots, then follow marks objects.
    {
        ScopedStopTheWorld stw("p10-root-mark-start", false);
        heap.old().mark_start();
    }
    heap.old().concurrent_mark();

    const bool watermarkDone = parked->GetStackWatermark().IsDone(StackWatermark::epoch_id());
    const bool live = fx.region0->is_object_strongly_live(from_object(held));
    std::fprintf(stderr, "P10_OLD_MARK_THREAD_ASSERT_EXECUTED live=%u done=%u epoch=%u\n",
                 unsigned(live), unsigned(watermarkDone), StackWatermark::epoch_id());
    GC_EXPECT_TRUE(live);
    GC_EXPECT_TRUE(watermarkDone);

    parked->PopNativeFrameRootsTo(frameMark);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
GC_OTHER_VM_TEST(YoungMarkStart, DoesNotParkPreviousFromPages)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    fx.region0->reset(PageAge::eden);
    fx.region0->SetRegionRole(ZPageRole::From);
    heap.young().pause_mark_start();
    const auto role = fx.region0->GetRegionRole();
    std::fprintf(stderr, "YOUNG_PAGE_PHASE_ASSERT executed=1 role=%u expected=%u\n",
                 unsigned(role), unsigned(ZPageRole::From));
    GC_EXPECT_EQ(role, ZPageRole::From);
}

// ZGC zGeneration.cpp:855-883 and zMark.cpp:853-891: mark-start
// publishes a new epoch; the concurrent root task consumes parked stacks.
GC_OTHER_VM_TEST(YoungMarkStart, ParkedRootDeferredToConcurrentMark)
{
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    fx.region0->reset(PageAge::eden);
    fx.obj0 = fx.PlaceObject(fx.region0->GetRegionStart() + 64);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(fx.obj0) + 64);
    Mutator* parked = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    parked->SetManagedContext(false);
    (void)parked->EnterSaferegion(false);
    const size_t frameMark = parked->NativeFrameRootCount();
    parked->AddNativeFrameRoot(fx.obj0);

    heap.young().pause_mark_start();
    const uint32_t epoch = StackWatermark::epoch_id();
    const bool doneAtStart = parked->GetStackWatermark().IsDone(epoch);
    const bool liveAtStart = fx.region0->is_object_strongly_live(from_object(fx.obj0));
    heap.young().concurrent_mark();
    const bool doneAfterRoots = parked->GetStackWatermark().IsDone(epoch);
    const bool liveAfterRoots = fx.region0->is_object_strongly_live(from_object(fx.obj0));
    parked->PopNativeFrameRootsTo(frameMark);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    std::fprintf(stderr, "YOUNG_ROOT_PHASE_ASSERT executed=1 start_done=%u start_live=%u concurrent_done=%u concurrent_live=%u\n",
                 unsigned(doneAtStart), unsigned(liveAtStart), unsigned(doneAfterRoots), unsigned(liveAfterRoots));
    GC_EXPECT_FALSE(doneAtStart);
    GC_EXPECT_FALSE(liveAtStart);
    GC_EXPECT_TRUE(doneAfterRoots);
    GC_EXPECT_TRUE(liveAfterRoots);
}

GC_OTHER_VM_TEST(NativeRootCurrent, ColoredAndNullBoundary)
{
    PrintNativeRootMaps();
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTest::BindNativeRootFixture(collector);
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
    RelocationReceiptTest::BindNativeRootFixture(collector);
    fx.region0->reset(PageAge::eden);
    Heap::GetHeap().young().mark_start();
    Heap::GetHeap().young().Mark().verify_all_stacks_empty();
    // Load-good, but the previous young/old mark epochs: the root must take
    // ZBarrier's mark-young slow path even though no remapping is needed.
    NativeSlot root(to_zpointer(raw(StoreGoodPointer(fx.obj0)) ^ ZPointerMarkedYoungMask ^ ZPointerMarkedOldMask));
    const size_t before = RelocationReceiptTest::PendingYoungRootWork(collector);
    ZBarrier::MarkYoungGoodBarrierOnOopField(root);
    const bool marked = fx.region0->is_object_strongly_live(from_object(fx.obj0));
    const size_t first = RelocationReceiptTest::PendingYoungRootWork(collector);
    std::fprintf(stderr, "B19_YOUNG_MARK_BEFORE_HEAL executed=1 marked=%u before=%zu after=%zu word=%#lx\n",
                 unsigned(marked), before, first, raw(root.GetFieldValue()));
    GC_EXPECT_TRUE(marked);
    GC_EXPECT_EQ(first, before + 1);
    GC_EXPECT_TRUE(ZPointer::is_marked_young(to_zpointer(raw(root.GetFieldValue()))));
    // The same physical, now young-good slot must not publish another follow.
    ZBarrier::MarkYoungGoodBarrierOnOopField(root);
    GC_EXPECT_EQ(RelocationReceiptTest::PendingYoungRootWork(collector), first);
    RelocationReceiptTest::DrainYoungRootWork(collector);
    GC_EXPECT_EQ(RelocationReceiptTest::PendingYoungRootWork(collector), size_t(0));
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
    RelocationReceiptTest::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(PageAge::old);
    // Seed the real scheduling input through its existing fixture operation.
    // The root task and marker below are the product TraceHeap implementation.
    GC_EXPECT_TRUE(FinalizerProcessorTest::Queue(Heap::GetHeap().GetFinalizerProcessor(), fixture.obj0));
    RelocationReceiptTest::NativeRootTrace(collector);
    const bool marked = fixture.region0->is_object_strongly_live(from_object(fixture.obj0));
    std::fprintf(stderr, "ROOT_STORAGE_STRONG_TARGET executed=1 object=%p marked=%u\n",
                 fixture.obj0, unsigned(marked));
    // Both observations are read before either target assertion can fail.
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
    RelocationReceiptTest::BindNativeRootFixture(collector, 2);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(PageAge::eden);
    auto& finalizers = heap.GetFinalizerProcessor();
    // More than two maximum-sized segments: oopStorage.cpp:1101 max_step=10.
    constexpr size_t count = 24 * sizeof(uintptr_t) * CHAR_BIT;
    for (size_t i = 0; i < count; ++i) {
        if (family == 0) { GC_EXPECT_TRUE(FinalizerProcessorTest::Queue(finalizers, fixture.obj0)); }
        else if (family == 1) { finalizers.RegisterFinalizer(fixture.obj0); }
        else { (void)heap.RegisterExportRoot(fixture.obj0); }
    }
    std::unordered_map<NativeSlot*, uintptr_t> before;
    const NativeSlotVisitor remember = [&](NativeSlot& slot) {
        before.emplace(&slot, raw(slot.GetFieldValue()));
    };
    if (family == 0) { finalizers.VisitGCRoots(remember); }
    else if (family == 1) { finalizers.VisitFinalizers(remember); }
    else { heap.VisitAllExportRoots(remember); }
    // ZGC zMark.cpp:877: the young root task consumes ALL colored root
    // storages. Its barrier recolors each slot after mark-start flips the
    // young mark bit. Read the stored word directly; an oop load would heal
    // it and hide a skipped task. Allocation-page implicit liveness cannot
    // satisfy this per-slot transition.
    RelocationReceiptTest::NativeRootMajorPrelude(collector);
    size_t remaining = 0;
    size_t transitioned = 0;
    bool valuesValid = true;
    const NativeSlotVisitor observe = [&](NativeSlot& slot) {
        const auto it = before.find(&slot);
        if (it == before.end()) { return; }
        ++remaining;
        const zpointer value = slot.GetFieldValue();
        transitioned += raw(value) != it->second && ZPointer::is_marked_young(value);
        valuesValid &= to_object(slot.GetTargetObject()) == fixture.obj0;
    };
    if (family == 0) { finalizers.VisitGCRoots(observe); }
    else if (family == 1) { finalizers.VisitFinalizers(observe); }
    else { heap.VisitAllExportRoots(observe); }
    const bool covered = before.size() == count && remaining == count;
    std::fprintf(stderr,
        "ROOT_SEGMENT_TARGET executed=1 family=%u slots=%zu remaining=%zu transitioned=%zu values_valid=%u\n",
        family, before.size(), remaining, transitioned, unsigned(valuesValid));
    GC_EXPECT_TRUE(covered && valuesValid && transitioned == count);
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
    RelocationReceiptTest::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    fixture.region0->reset(PageAge::eden);
    std::vector<U64> original;
    for (size_t i = 0; i < sizeof(uintptr_t) * CHAR_BIT; ++i) {
        original.push_back(heap.RegisterExportRoot(fixture.obj0));
    }
    auto& storage = heap.GetExportRootStorage();
    const size_t before = OopStorageTest::BlockCount(storage);
    RelocationReceiptTest::NativeRootMajorPrelude(collector);
    const size_t afterScan = OopStorageTest::BlockCount(storage);
    const U64 added = heap.RegisterExportRoot(fixture.obj0);
    const size_t grown = OopStorageTest::BlockCount(storage);
    for (U64 handle : original) { heap.RemoveExportObject(handle); }
    const size_t after = OopStorageTest::BlockCount(storage);
    const size_t remaining = storage.AllocationCount();
    std::fprintf(stderr,
        "ROOT_LIFETIME_TARGET executed=1 before=%zu after_scan=%zu grown=%zu after=%zu remaining=%zu\n",
        before, afterScan, grown, after, remaining);
    heap.RemoveExportObject(added);
    GC_EXPECT_TRUE(afterScan == before && grown >= before && remaining >= 1);
}
#endif

// ZGC zMark.cpp:703-708: callers finish every thread; the watermark owns
// completion. Exercise the exported product root phase and handshake entries.
namespace {
void CheckYoungThreadCompletion(bool handshakeFirst)
{
    using namespace MapleRuntime;
    using namespace MapleRuntime::GcUnit;
    B09RuntimeFixture runtime;
    GcHeapFixture fixture;
    fixture.region0->reset(PageAge::eden);
    MarkPublicationFixture marking;
    Mutator* thread = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    thread->SetManagedContext(false);
    (void)thread->EnterSaferegion(false);
    const size_t frameMark = thread->NativeFrameRootCount();
    ObjectRef* root = thread->AddNativeFrameRoot(fixture.obj0);
    const uint64_t epoch = StackWatermark::epoch_id();
    const bool initiallyDone = thread->GetStackWatermark().IsDone(epoch);
    const bool initiallyLive = fixture.region0->is_object_strongly_live(from_object(fixture.obj0));
    size_t published = 0;
    auto readPublished = [&] {
        marking.DrainDomain(*Heap::GetHeap().young().MarkPtr(), [&](BaseObject* object, bool follow) {
            GC_EXPECT_TRUE(object == fixture.obj0 && follow);
            ++published;
        });
    };
    if (handshakeFirst) {
        (void)thread->GcPhaseEnum(true, epoch, false);
        const bool done = thread->GetStackWatermark().IsDone(epoch);
        readPublished();
        std::fprintf(stderr, "YOUNG_HANDSHAKE_TARGET done=%u published=%zu initially_done=%u initially_live=%u\n",
                     unsigned(done), published, unsigned(initiallyDone), unsigned(initiallyLive));
        GC_EXPECT_TRUE(!initiallyDone && !initiallyLive && done && published == 1);
    }
    size_t rootResults = 0;
    auto observe = [&](BaseObject* object) { if (object == fixture.obj0) ++rootResults; };
    ZMark::VisitMinorRoots(observe, observe);
    const bool firstDone = thread->GetStackWatermark().IsDone(epoch);
    readPublished();
    const size_t firstPublished = published;
    const size_t firstResults = rootResults;
    ZMark::VisitMinorRoots(observe, observe);
    readPublished();
    const bool sameRoot = raw(root->LoadPlain()) == reinterpret_cast<uintptr_t>(fixture.obj0);
    std::fprintf(stderr, "YOUNG_THREAD_COMPLETION_TARGET handshake=%u done=%u published=%zu first=%zu second=%zu same=%u\n",
                 unsigned(handshakeFirst), unsigned(firstDone), published, firstResults, rootResults,
                 unsigned(sameRoot));
    GC_EXPECT_TRUE(firstDone && firstPublished == 1 && published == firstPublished && sameRoot);
    GC_EXPECT_EQ(firstResults, handshakeFirst ? size_t(0) : size_t(1));
    GC_EXPECT_EQ(rootResults, firstResults);
    thread->PopNativeFrameRootsTo(frameMark);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
}
GC_OTHER_VM_TEST(YoungThreadRoots, UnfinishedThreadCompletesOnce) { CheckYoungThreadCompletion(false); }
GC_OTHER_VM_TEST(YoungThreadRoots, HandshakeCompletedThreadIsIdempotent) { CheckYoungThreadCompletion(true); }
