// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "gc_generation_test.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Mutator/ThreadLocal.h"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zWorkers.hpp"
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ZGC zRelocate.cpp:906-927,1010-1047. Saturate the real page allocator,
// then invoke the product relocation phase with two sparse old source pages.
// Mark bits/selection are fixture inputs; target choice, copying, page-table
// removal and physical accounting are exclusively produced by the runtime SO.
static void CheckInPlaceTargets(bool medium, bool promote, uint32_t workers)
{
    CreateStandaloneHeap(medium ? 4 : 2);
    if (medium) {
        ZHeuristics::set_max_heap_size(128 * 1024 * 1024);
        ZHeuristics::set_medium_page_size();
    }
    const size_t pageSize = medium ? ZPageSizeMediumMax : ZPageSizeSmall;
    const size_t objectSize = medium ? ZObjectAlignmentMedium : 24;
    const PageAge age = promote ? PageAge::survivor1 : PageAge::old;
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& manager = heap.page_allocator();
    ZGeneration& generation = promote ? static_cast<ZGeneration&>(heap.young())
                                     : static_cast<ZGeneration&>(heap.old());
    generation.InitializeWorkers(workers);
    generation.Workers()->set_active_workers(workers);
    generation.Begin(1);
    GenerationSequenceFixture::Advance(generation);
    if (promote) { ZGenerationTest::SetTenuringThreshold(heap.young(), 1); }

    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(objectSize - sizeof(uintptr_t));
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* pages[2];
    MAddress starts[2];
    MAddress objects[2];
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(pageSize, medium ? ZPageType::medium : ZPageType::small, false, false, true, age, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        starts[i] = pages[i]->GetRegionStart();
        objects[i] = pages[i]->alloc_object(objectSize);
        auto* dead = reinterpret_cast<BaseObject*>(objects[i]);
        dead->SetClassInfo(type);
        objects[i] = pages[i]->alloc_object(objectSize);
        auto* object = reinterpret_cast<BaseObject*>(objects[i]);
        object->SetClassInfo(type);
        *reinterpret_cast<uint64_t*>(objects[i] + 8) = 0x796000 + i;
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], object));
    }
    GC_EXPECT_EQ(manager.GetUsedBytes(), 2 * pageSize);
    ZRelocationSetSelector selector;
    for (ZPage* page : pages) {
        ZPageTest::MakeRelocatable(*page);
        selector.register_live_page(page);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owners[2] = {forwarding_for_page(pages[0]), forwarding_for_page(pages[1])};
    GC_EXPECT_TRUE(owners[0] != nullptr && owners[1] != nullptr);
    generation.set_phase(ZGenerationPhase::Relocate);
    generation.relocate().relocate(&generation.relocation_set());
    const MAddress destinations[2] = {owners[0]->find(objects[0]), owners[1]->find(objects[1])};
    GC_EXPECT_TRUE(destinations[0] != 0 && destinations[1] != 0);
    ZPage* target0 = Heap::page(destinations[0]);
    ZPage* target1 = Heap::page(destinations[1]);
    const size_t retired = (Heap::page(starts[0]) == nullptr) + (Heap::page(starts[1]) == nullptr);
    std::fprintf(stderr,
        "INPLACE_TARGET_RESULT target0=%p target1=%p top_bytes=%zu retired=%zu used=%zu payload0=%llu payload1=%llu\n",
        target0, target1, target0->GetRegionAllocatedSize(), retired, manager.GetUsedBytes(),
        static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(destinations[0] + 8)),
        static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(destinations[1] + 8)));
    GC_EXPECT_TRUE(target0 == target1);
    GC_EXPECT_EQ(target0->GetRegionAllocatedSize(), 2 * objectSize);
    GC_EXPECT_TRUE(target0->age() == PageAge::old);
    GC_EXPECT_EQ(retired, 1u);
    GC_EXPECT_EQ(manager.GetUsedBytes(), pageSize);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destinations[0] + 8), 0x796000u);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destinations[1] + 8), 0x796001u);
}

GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, InPlaceTargetReusedAndSourceFreed)
{
    CheckInPlaceTargets(false, false, 1);
}
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, PromotedInPlaceTargetReusedAndSourceFreed)
{
    CheckInPlaceTargets(false, true, 1);
}
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, MediumInPlaceTargetSharedAcrossWorkers)
{
    CheckInPlaceTargets(true, false, 2);
}

// ZGC zRelocate.cpp:977-985,1031: preserve relocated current bits, clear previous.
extern "C" int CJ_ScheduleManagerInit();

namespace {
// Runtime container only; pause and relocation execute the product methods.
class InPlaceRemsetRuntime final : public Runtime {
public:
    explicit InPlaceRemsetRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        runtime = this;
    }
    ~InPlaceRemsetRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
};
}

static void CheckInPlaceRemset(bool eager)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutators;
    InPlaceRemsetRuntime runtime(mutators);
    const bool medium=false, promote=false; const uint32_t workers=1;
    CreateStandaloneHeap(medium ? 4 : 2);
    if (medium) {
        ZHeuristics::set_max_heap_size(128 * 1024 * 1024);
        ZHeuristics::set_medium_page_size();
    }
    const size_t pageSize = medium ? ZPageSizeMediumMax : ZPageSizeSmall;
    const size_t objectSize = medium ? ZObjectAlignmentMedium : 24;
    const PageAge age = promote ? PageAge::survivor1 : PageAge::old;
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& manager = heap.page_allocator();
    ZGeneration& generation = promote ? static_cast<ZGeneration&>(heap.young())
                                     : static_cast<ZGeneration&>(heap.old());
    generation.InitializeWorkers(workers);
    generation.Workers()->set_active_workers(workers);
    generation.Begin(1);
    generation.RecordYoungSequenceAtRelocateStart(heap.young().Sequence());
    GenerationSequenceFixture::Advance(generation);
    if (promote) { ZGenerationTest::SetTenuringThreshold(heap.young(), 1); }

    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(objectSize - sizeof(uintptr_t));
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT | 1; // one reference field at payload offset 0
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* pages[2];
    MAddress starts[2];
    MAddress objects[2];
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(pageSize, medium ? ZPageType::medium : ZPageType::small, false, false, true, age, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        starts[i] = pages[i]->GetRegionStart();
        objects[i] = pages[i]->alloc_object(objectSize);
        auto* dead = reinterpret_cast<BaseObject*>(objects[i]);
        dead->SetClassInfo(type);
        objects[i] = pages[i]->alloc_object(objectSize);
        auto* object = reinterpret_cast<BaseObject*>(objects[i]);
        object->SetClassInfo(type);
        *reinterpret_cast<uint64_t*>(objects[i] + 8) = 0; // valid null reference field
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], object));
    }
    GC_EXPECT_EQ(manager.GetUsedBytes(), 2 * pageSize);
    pages[0]->remember(reinterpret_cast<volatile zpointer*>(objects[0]+8));
    GC_EXPECT_FALSE(pages[0]->is_remset_cleared_current());
    GC_EXPECT_TRUE(pages[0]->is_remset_cleared_previous());
    GC_EXPECT_TRUE(heap.OldActiveRemsetIsCurrent());
    ZRelocationSetSelector selector;
    for (ZPage* page : pages) {
        ZPageTest::MakeRelocatable(*page);
        selector.register_live_page(page);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owners[2] = {forwarding_for_page(pages[0]), forwarding_for_page(pages[1])};
    GC_EXPECT_TRUE(owners[0] != nullptr && owners[1] != nullptr);
    generation.set_phase(ZGenerationPhase::Relocate);
    if (eager) {
        ScopedStopTheWorld pause("in-place remset completion", false);
        BaseObject* result = generation.relocate().relocate_object(
            owners[0], reinterpret_cast<BaseObject*>(objects[0]), ForwardingProvenance{});
        GC_EXPECT_TRUE(result == reinterpret_cast<BaseObject*>(owners[0]->find(objects[0])));
    }
    else { generation.relocate().relocate(&generation.relocation_set()); }
    std::fprintf(stderr, "REMSET_RESULT eager=%d current_clear=%d previous_clear=%d done=%d\n", eager, pages[0]->is_remset_cleared_current(), pages[0]->is_remset_cleared_previous(), owners[0]->is_done());
    GC_EXPECT_TRUE(pages[0]->is_remset_cleared_previous());
    GC_EXPECT_FALSE(pages[0]->is_remset_cleared_current());
    GC_EXPECT_TRUE(owners[0]->is_done());
    const MAddress destination = owners[0]->find(objects[0]);
    GC_EXPECT_EQ(destination, starts[0]);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destination + 8), 0u);
}
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, EagerInPlaceClearsPreviousRemset) { CheckInPlaceRemset(true); }
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, WorkerInPlaceClearsPreviousRemset) { CheckInPlaceRemset(false); }
