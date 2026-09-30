#include "gc_worker_fixture.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "ObjectModel/MObject.h"
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
#include "Heap/z/zUncoloredRoot.inline.hpp"
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
        Heap::GetHeap().old().remap_young_roots();
    }
    static void BindNativeRootFixture(Heap& collector, uint32_t workers = 1)
    {
        CHECK(&collector == &Heap::GetHeap());
        ZCollectedHeapTest::SetWorkers(workers);
        for (auto gen : {ZGenerationId::young, ZGenerationId::old}) {
            auto& cycle = Heap::GetHeap().GetZGeneration(gen);
            if (cycle.Workers() == nullptr) {
                MapleRuntime::GcUnit::InitializeGenerationWorkers(cycle, workers);
            } else {
                cycle.Workers()->set_active_workers(workers);
            }
        }
        ZGlobalsPointers::initialize();
    }
    static void FlipNativeRootYoung(Heap& collector) { ZGlobalsPointers::flip_young_relocate_start(); }
    static void NativeRootMajorPrelude(Heap& collector)
    {
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        ZGeneration::young()->collect(ZYoungType::major_partial_roots);
    }
    static void NativeRootTrace(Heap& collector)
    {
        // ZGC zGeneration.cpp:1212-1237: use the product mark-start to
        // establish colors and sequence before the real concurrent root task.
        auto& old = Heap::GetHeap().old();
        {
            ScopedStopTheWorld stopped("native-root old mark-start");
            old.mark_start();
        }
        old.concurrent_mark();
    }
    static size_t PendingYoungRootWork(Heap& collector)
    {
        return ThreadLocal::GetMarkStacks(*Heap::GetHeap().young().MarkPtr()).Population();
    }
    static void DrainYoungRootWork(Heap& collector)
    {
        (void)ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *Heap::GetHeap().young().MarkPtr());
        Heap::GetHeap().young().Mark().MarkFollow();
    }
};
}

#include "Heap/z/zAccess.hpp"

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
// resolve before marker publication. The relocation worker supplies the actual to;
// no forwarding mapping or consumer argument is manufactured by this test.
void CheckNativeRoot(bool minor, unsigned threadKind = 0)
{
    CreateStandaloneHeap(GcHeapFixture::kUnits);
    PrintNativeRootMaps();
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    Heap& collector = heap;
    RelocationReceiptTest::BindNativeRootFixture(collector);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* region = fx.region0();
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
    zaddress_unsafe invisibleMem = to_zaddress_unsafe(reinterpret_cast<uintptr_t>(from));
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
            thread->GetGCData().set_invisible_root(&invisibleMem);
            historicalSlot = reinterpret_cast<RootSlot*>(&invisibleMem);
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
    // Two real sparse pages satisfy the selector's strict reclaimable-page test.
    fx.region1()->reset(PageAge::eden);
    BaseObject* companion = fx.PlaceObject(fx.region1()->GetRegionStart());
    fx.region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(companion) + companion->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1(), companion));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {region, fx.region1()}));
    Heap::GetHeap().GetZGeneration(ZGenerationId::young).set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::FlipNativeRootYoung(collector);
    if (heap.young().Workers() == nullptr) { MapleRuntime::GcUnit::InitializeGenerationWorkers(heap.young(), 1); }
    heap.young().Workers()->set_active_workers(1);
    ZRelocate::StartRelocationTasks(heap.young().id());
    const MAddress forwardStart = region->GetRegionStart();
    const Generation forwardGeneration = region->GetOwnerGeneration();
    heap.young().relocate().relocate(&heap.young().relocation_set());
    // zForwardingTable.inline.hpp:43. Do not pass the pre-relocate descriptor
    // into forwarding_for_page: non-in-place completion frees it (zRelocate.cpp:450).
    auto forwarding = Heap::GetHeap().GetZGeneration(forwardGeneration).forwarding_table().get(forwardStart);
    BaseObject* to = reinterpret_cast<BaseObject*>(forwarding->find(reinterpret_cast<MAddress>(from)));
    // Eden advances to survivor1 at this threshold. Preserve the fixture's
    // explicit promotion so the old root task observes an actual old page.
    // clone_for_promotion needs the live from-page. flip_promote's replace
    // (zPageTable.cpp:61) requires that page to still be the table entry, so
    // this relocate did not take free_page; the descriptor was retained
    // in place (zRelocate.cpp:438-448).
    ZPage* promoted = region->clone_for_promotion();
    heap.young().flip_promote(region, promoted);
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
        if (threadKind == 3) thread->GetGCData().clear_invisible_root();
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
void CheckSavedRootColor(bool invisible, bool twoRounds = false)
{
    CreateStandaloneHeap(GcHeapFixture::kUnits);
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0();
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
    zaddress_unsafe invisibleMem = to_zaddress_unsafe(reinterpret_cast<uintptr_t>(from));
    if (invisible) {
        thread->GetGCData().set_invisible_root(&invisibleMem);
        slot = reinterpret_cast<RootSlot*>(thread->GetGCData().invisible_root());
    } else {
        slot = thread->AddNativeFrameRoot(from);
    }
    if (twoRounds) {
        ZGlobalsPointers::flip_old_relocate_start();
        StackWatermarkSet::finish_processing(*thread, reinterpret_cast<void*>(ZUncoloredRoot::mark));
        const bool first = thread->GetStackWatermark().IsDone();
        GC_EXPECT_TRUE(first);
        GC_EXPECT_EQ(raw(slot->LoadPlain()), reinterpret_cast<uintptr_t>(from));
    }
    const uintptr_t savedColor = thread->GetGCData().loadGoodMask;
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, earlier));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, from));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(page, second));
    // Two real sparse pages satisfy the selector's strict reclaimable-page test.
    fx.region1()->reset(PageAge::eden);
    BaseObject* companion = fx.PlaceObject(fx.region1()->GetRegionStart());
    fx.region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(companion) + companion->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1(), companion));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {page, fx.region1()}));
    heap.young().set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::FlipNativeRootYoung(heap);
    // Compact via the product implementation. Keep another live object at the
    // old address so a color cut reaches the address assertion, not an invalid
    // header. The earlier live object also makes a duplicate remap return a
    // valid but wrong object, exposing double consumption at that assertion.
    // Neither the forwarding value nor the root result is fabricated.
    if (heap.young().Workers() == nullptr) { MapleRuntime::GcUnit::InitializeGenerationWorkers(heap.young(), 1); }
    heap.young().Workers()->set_active_workers(1);
    ZRelocate::StartRelocationTasks(heap.young().id());
    const MAddress forwardStart = page->GetRegionStart();
    const Generation forwardGeneration = page->GetOwnerGeneration();
    heap.young().relocate().relocate(&heap.young().relocation_set());
    const MAddress expected = Heap::GetHeap().GetZGeneration(forwardGeneration).forwarding_table().get(forwardStart)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    StackWatermarkSet::finish_processing(*thread, reinterpret_cast<void*>(ZUncoloredRoot::mark));
    const bool scanned = thread->GetStackWatermark().IsDone();
    const uintptr_t observed = raw(slot->LoadPlain());
    std::fprintf(stderr,
        "SAVED_ROOT_COLOR_TARGET invisible=%u scanned=%u saved=%#lx current=%#lx from=%p observed=%#lx expected=%#lx\n",
        unsigned(invisible), unsigned(scanned), savedColor, ZPointerLoadGoodMask, from, observed, expected);
    // Evaluate the product result before cleanup can consume it again.
    GC_EXPECT_EQ(observed, expected);
    GC_EXPECT_TRUE(scanned);
    if (invisible) { thread->GetGCData().clear_invisible_root(); }
    thread->PopNativeFrameRootsTo(roots);
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}
}
GC_COMPONENT_OTHER_VM_TEST(ThreadRootCurrent, TwoEpochNativeFrameRoot) { CheckSavedRootColor(false, true); }
GC_COMPONENT_OTHER_VM_TEST(ThreadRootCurrent, TwoEpochInvisibleRoot) { CheckSavedRootColor(true, true); }
GC_COMPONENT_OTHER_VM_TEST(ThreadRootCurrent, SavedColorNativeFrameRoot) { CheckSavedRootColor(false); }
GC_COMPONENT_OTHER_VM_TEST(ThreadRootCurrent, SavedColorInvisibleRoot) { CheckSavedRootColor(true); }

GC_COMPONENT_OTHER_VM_TEST(ThreadRootCurrent, RemapYoungRootsNativeFrameRoot)
{
    CreateStandaloneHeap(GcHeapFixture::kUnits);
    B09RuntimeFixture runtime;
    GcHeapFixture fx;
    auto& heap = Heap::GetHeap();
    RelocationReceiptTest::BindNativeRootFixture(heap);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    ZPage* page = fx.region0();
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
    // Two real sparse pages satisfy the selector's strict reclaimable-page test.
    fx.region1()->reset(PageAge::eden);
    BaseObject* companion = fx.PlaceObject(fx.region1()->GetRegionStart());
    fx.region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(companion) + companion->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1(), companion));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Young, {page, fx.region1()}));
    heap.young().set_phase(ZGenerationPhase::Relocate);
    RelocationReceiptTest::FlipNativeRootYoung(heap);
    if (heap.young().Workers() == nullptr) { MapleRuntime::GcUnit::InitializeGenerationWorkers(heap.young(), 1); }
    heap.young().Workers()->set_active_workers(1);
    ZRelocate::StartRelocationTasks(heap.young().id());
    const MAddress forwardStart = page->GetRegionStart();
    const Generation forwardGeneration = page->GetOwnerGeneration();
    heap.young().relocate().relocate(&heap.young().relocation_set());
    const MAddress expected = Heap::GetHeap().GetZGeneration(forwardGeneration).forwarding_table().get(forwardStart)->find(reinterpret_cast<MAddress>(from));
    GC_EXPECT_TRUE(expected != 0 && expected != reinterpret_cast<MAddress>(from));
    Heap::GetHeap().old().remap_young_roots();
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
    fx.region1()->reset(PageAge::old);
    BaseObject* held = fx.PlaceObject(fx.region1()->GetRegionStart());
    fx.region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(held) + held->GetSize());
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region1(), held));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(fx.region0(), fx.obj0));
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {fx.region0(), fx.region1()}));
    GC_EXPECT_TRUE(!ZGeneration::generation(fx.region1()->generation_id())->forwarding(fx.region1()->GetRegionStart())->is_done());
    GC_EXPECT_TRUE(ZGeneration::generation((fx.region1())->generation_id())->forwarding((fx.region1())->GetRegionStart()) != nullptr);
    heap.young().set_phase(ZGenerationPhase::Relocate);
    ZRelocate::StartRelocationTasks(ZGenerationId::young);
    heap.young().Relocate();
    GC_EXPECT_TRUE(forwarding_for_page(fx.region1()) != nullptr);
    GC_EXPECT_TRUE(!forwarding_for_page(fx.region1())->is_done());
