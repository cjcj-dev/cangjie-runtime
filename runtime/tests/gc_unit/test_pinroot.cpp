// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "gc_generation_test.hpp"
#include "b09_runtime_fixture.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Mutator/ThreadLocal.h"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "CangjieRuntime.h"
#include "Mutator/Mutator.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include <cstdio>
#include <chrono>
#include <cstring>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// ZGC zRelocate.cpp:906-927,1010-1047. Saturate the real page allocator,
// then invoke the product relocation phase with two sparse old source pages.
// Mark bits/selection are fixture inputs; target choice, copying, page-table
// removal and physical accounting are exclusively produced by the runtime SO.
static void CheckInPlaceTargets(bool medium, bool promote, uint32_t workers, bool retain = false)
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
        pages[i] = Heap::alloc_page(pageSize, medium ? ZPageType::medium : ZPageType::small, false, age, flags);
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
    ZRelocationSetSelector selector(0.0);
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
    if (retain) {
        GC_EXPECT_TRUE(owners[0]->retain_page(generation.relocate().queue()) &&
            owners[1]->retain_page(generation.relocate().queue()));
        ZRelocate::StartRelocationTasks(generation.id());
        std::thread worker([&] { generation.relocate().relocate(&generation.relocation_set()); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool claimedBeforeCopy = false;
        int32_t counts[2]{};
        MAddress published[2]{};
        do {
            counts[0] = owners[0]->ref_count().load(std::memory_order_acquire);
            counts[1] = owners[1]->ref_count().load(std::memory_order_acquire);
            published[0] = owners[0]->find(objects[0]);
            published[1] = owners[1]->find(objects[1]);
            claimedBeforeCopy = (counts[0] < 0 || counts[1] < 0) && published[0] == 0 && published[1] == 0;
            if (claimedBeforeCopy || published[0] != 0 || published[1] != 0) { break; }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        owners[0]->release_page();
        owners[1]->release_page();
        worker.join();
        std::fprintf(stderr, "INPLACE_CLAIM_RESULT refs0=%d refs1=%d published0=%zx published1=%zx "
            "claimed_before_copy=%d\n", counts[0], counts[1], published[0], published[1], claimedBeforeCopy);
        GC_EXPECT_TRUE(claimedBeforeCopy);
    } else {
        ZRelocate::StartRelocationTasks(generation.id());
        generation.relocate().relocate(&generation.relocation_set());
    }
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

GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, InPlaceClaimsBeforeCopyWithRetainedPage)
{
    CheckInPlaceTargets(false, false, 1, true);
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

static void CheckInPlaceRemset()
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
        pages[i] = Heap::alloc_page(pageSize, medium ? ZPageType::medium : ZPageType::small, false, age, flags);
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
    ZRelocationSetSelector selector(0.0);
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
    ZRelocate::StartRelocationTasks(generation.id());
    generation.relocate().relocate(&generation.relocation_set());
    std::fprintf(stderr, "REMSET_RESULT current_clear=%d previous_clear=%d done=%d\n",
        pages[0]->is_remset_cleared_current(), pages[0]->is_remset_cleared_previous(), owners[0]->is_done());
    GC_EXPECT_TRUE(pages[0]->is_remset_cleared_previous());
    GC_EXPECT_FALSE(pages[0]->is_remset_cleared_current());
    GC_EXPECT_TRUE(owners[0]->is_done());
    const MAddress destination = owners[0]->find(objects[0]);
    GC_EXPECT_EQ(destination, starts[0]);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destination + 8), 0u);
}
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, WorkerInPlaceClearsPreviousRemset) { CheckInPlaceRemset(); }

// ZGC zRelocate.cpp:382-410: a successful mutator relocation publishes only
// the requested object, irrespective of the caller's safepoint state. Spare
// capacity makes allocation success deterministic; the worker failure/reuse
// cases above exercise the separate allocation-failure in-place route.
static void CheckMutatorRelocation(bool stopped)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutators;
    InPlaceRemsetRuntime runtime(mutators);
    CreateStandaloneHeap(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& generation = heap.old();
    GenerationSequenceFixture::Advance(generation);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(16);
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZRelocationSetSelector selector(0.0);
    ZPage* pages[2];
    BaseObject* objects[2][2];
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::old, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        for (size_t j = 0; j < 2; ++j) {
            objects[i][j] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(24));
            objects[i][j]->SetClassInfo(type);
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(objects[i][j]) + 8) = 0x909 + j;
            GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], objects[i][j]));
        }
        ZPageTest::MakeRelocatable(*pages[i]);
        selector.register_live_page(pages[i]);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owner = forwarding_for_page(pages[0]);
    GC_EXPECT_TRUE(owner != nullptr);
    generation.set_phase(ZGenerationPhase::Relocate);
    auto relocate = [&] {
        BaseObject* result = generation.relocate().relocate_object(owner, objects[0][0]);
        const MAddress other = owner->find(reinterpret_cast<MAddress>(objects[0][1]));
        // Print the product results before the target assertion, including the
        // positive control that distinguishes a skipped call from a real copy.
        std::fprintf(stderr, "MUTATOR_RELOCATE_RESULT stopped=%d world_stopped=%d source=%p result=%p "
            "in_place=%d other=%zx done=%d refs=%d\n", stopped, mutators.WorldStopped(),
            objects[0][0], result, owner->in_place(), other, owner->is_done(),
            owner->ref_count().load(std::memory_order_acquire));
        GC_EXPECT_FALSE(owner->in_place());
        GC_EXPECT_TRUE(result != nullptr && result != objects[0][0]);
        GC_EXPECT_EQ(owner->find(reinterpret_cast<MAddress>(objects[0][0])), reinterpret_cast<MAddress>(result));
        GC_EXPECT_EQ(other, 0u);
        GC_EXPECT_FALSE(owner->is_done());
        GC_EXPECT_EQ(owner->ref_count().load(std::memory_order_acquire), 1);
        GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(result) + 8), 0x909u);
    };
    if (stopped) {
        ScopedStopTheWorld pause("mutator relocation routing", false);
        relocate();
    } else {
        relocate();
    }
}

GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, StoppedWorldCopiesOnlyRequestedObject)
{
    CheckMutatorRelocation(true);
}
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, RunningWorldCopiesOnlyRequestedObject)
{
    CheckMutatorRelocation(false);
}

#if defined(__x86_64__) && defined(__linux__)
namespace {
struct FrameRootMapImage {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t descriptorRest[6];
    uint8_t bits[256];
};
FrameRootMapImage frameRootMapImage;

void InitializeFrameRootMap()
{
    auto& image = frameRootMapImage;
    std::memset(&image, 0, sizeof(image));
    image.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&image.stackMapOffset) -
        reinterpret_cast<char*>(&image.descriptorOffset));
    image.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(image.bits) -
        reinterpret_cast<char*>(&image.stackMapOffset));
    ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(image.pc));
    size_t bit = 0;
    auto put = [&](uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            image.bits[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1u) << (bit % 8));
        }
    };
    auto var = [&](uint32_t value) {
        if (value <= 11) { put(value, 4); }
        else { put(12, 4); put(value, 8); }
    };
    // R13 is callee-saved index 2 (RegisterX86-64.h:78). Spill offset 2 → fp-16.
    // PC 0 has no reg root so the younger frame can RecordCalleeSaved first.
    // PC 16 names R13; VisitSingleSlotsRoot then heals that spill.
    var(0); var(0); var(4); var(2);
    var(2); var(4); var(1); var(1); var(1);
    if (CangjieRuntime::stackGrowConfig == StackGrowConfig::STACK_GROW_ON) { var(0); var(0); }
    var(0);
    put(0, 32); put(0, 4); put(0, 1); put(0, 1); put(0, 1);
    put(16, 32); put(1, 4); put(0, 1); put(0, 1); put(0, 1);
    var(1); var(16);
    put(1u << 13, 16);
    var(0); var(8); var(0);
    var(0); var(0);
    var(0);
}
}
#endif

// zUncoloredRoot.inline.hpp:62-68 and zGeneration.inline.hpp:131-139: relocate-start
// exit processing writes the to-address of a cset frame slot before concurrent relocate.
static void CheckRelocateStartExitRemapsFrameRoot()
{
    B09RuntimeFixture runtime;
    CreateStandaloneHeap(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& generation = heap.old();
    GenerationSequenceFixture::Advance(generation);
    if (heap.young().Workers() == nullptr) {
        heap.young().InitializeWorkers(1);
    }
    heap.young().Workers()->set_active_workers(1);
    if (generation.Workers() == nullptr) {
        generation.InitializeWorkers(1);
    }
    generation.Workers()->set_active_workers(1);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(16);
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZRelocationSetSelector selector(0.0);
    ZPage* pages[2];
    BaseObject* objects[2][2];
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::old, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        for (size_t j = 0; j < 2; ++j) {
            objects[i][j] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(24));
            objects[i][j]->SetClassInfo(type);
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(objects[i][j]) + 8) = 0x1104 + j;
            GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], objects[i][j]));
        }
        ZPageTest::MakeRelocatable(*pages[i]);
        selector.register_live_page(pages[i]);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owner = forwarding_for_page(pages[0]);
    GC_EXPECT_TRUE(owner != nullptr);
#if defined(__x86_64__) && defined(__linux__)
    InitializeFrameRootMap();
    const uintptr_t startIP = reinterpret_cast<uintptr_t>(frameRootMapImage.pc);
    uintptr_t younger[8] = {};
    uintptr_t caller[8] = {};
    younger[2] = reinterpret_cast<uintptr_t>(objects[0][0]);
    younger[3] = startIP + 9;
    younger[4] = reinterpret_cast<uintptr_t>(&caller[4]);
    younger[5] = startIP + 16;
    caller[3] = startIP + 9;
    Mutator* parked = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    GC_EXPECT_TRUE(parked != nullptr);
    parked->SetManagedContext(true);
    (void)parked->EnterSaferegion(false);
    if (parked->GetGCData().storeGoodMask == 0) {
        parked->GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
    }
    auto& context = parked->GetUnwindContext();
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(startIP));
    context.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(&younger[4]));
    context.anchorFA = nullptr;
    const uintptr_t before = younger[2];
    GC_EXPECT_EQ(before, reinterpret_cast<uintptr_t>(objects[0][0]));
    GC_EXPECT_EQ(owner->find(reinterpret_cast<MAddress>(objects[0][0])), static_cast<MAddress>(0));
    ZGlobalsPointers::flip_old_relocate_start();
    generation.set_phase(ZGenerationPhase::Relocate);
    StackWatermarkSet::on_safepoint(*parked);
    const uintptr_t after = younger[2];
    const MAddress relocated = owner->find(reinterpret_cast<MAddress>(objects[0][0]));
    std::fprintf(stderr,
        "FRAME_ROOT_REMAP_ASSERT_EXECUTED startIP=%#zx before=%#zx after=%#zx relocated=%#zx reg=r13\n",
        startIP, before, after, relocated);
    GC_EXPECT_EQ(after, relocated);
    GC_EXPECT_TRUE(relocated != 0 && relocated != reinterpret_cast<MAddress>(objects[0][0]));
    heap.young().pause_mark_start();
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
#else
    (void)owner;
    GC_EXPECT_TRUE(false);
#endif
}

GC_COMPONENT_OTHER_VM_TEST(RelocateStartFrameRoot, WritesToAddressBeforeConcurrentRelocate)
{
    CheckRelocateStartExitRemapsFrameRoot();
}

#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// The unmarked object has valid metadata: omitting the product precondition
// must complete relocation, rather than fail in an unrelated size reader.
void RunRelocateLiveness(bool worker, bool marked)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager mutators;
    InPlaceRemsetRuntime runtime(mutators);
    CreateStandaloneHeap(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& generation = heap.old();
    generation.InitializeWorkers(1);
    generation.Workers()->set_active_workers(1);
    GenerationSequenceFixture::Advance(generation);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(16);
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* pages[2];
    BaseObject* live[2];
    BaseObject* dead[2];
    ZRelocationSetSelector selector(0.0);
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::old, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        dead[i] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(24));
        live[i] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(24));
        for (auto* object : {dead[i], live[i]}) {
            object->SetClassInfo(type);
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(object) + 8) = 0x869;
        }
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], live[i]));
        ZPageTest::MakeRelocatable(*pages[i]);
        GC_EXPECT_TRUE(heap.IsSurvivedObject(live[i]));
        GC_EXPECT_FALSE(heap.IsSurvivedObject(dead[i]));
        selector.register_live_page(pages[i]);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owner = forwarding_for_page(pages[0]);
    GC_EXPECT_TRUE(owner != nullptr);
    generation.set_phase(ZGenerationPhase::Relocate);
    BaseObject* source = marked ? live[0] : dead[0];
    BaseObject* result;
    if (worker) {
        ZRelocate::StartRelocationTasks(generation.id());
        generation.relocate().relocate(&generation.relocation_set());
        result = reinterpret_cast<BaseObject*>(owner->find(reinterpret_cast<uintptr_t>(source)));
    } else {
        result = generation.relocate().relocate_object(owner, source);
    }
    GC_EXPECT_TRUE(result != nullptr && result != source);
    const PageAge resultAge = Heap::page(reinterpret_cast<uintptr_t>(result))->age();
    std::fprintf(stderr, "RELOCATION_AGE_TARGET worker=%d actual=%u expected=%u\n",
                 worker, unsigned(untype(resultAge)), unsigned(untype(PageAge::old)));
    GC_EXPECT_TRUE(resultAge == PageAge::old);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(result) + 8), 0x869u);
    std::fprintf(stderr, "RELOCATE_LIVE_RESULT worker=%d marked=%d source=%p result=%p payload=0x869\n",
                 worker, marked, source, result);
}

void CheckRelocateLiveness(bool worker, bool marked)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        RunRelocateLiveness(worker, marked);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool target = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("IsSurvivedObject(obj)") != std::string::npos &&
        transcript.find("Should be live") != std::string::npos;
    std::fprintf(stderr, "RELOCATE_LIVE_ASSERT worker=%d marked=%d status=%d target=%d\n",
                 worker, marked, status, target);
    if (!marked) {
        GC_EXPECT_TRUE(target);
    } else {
        GC_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        GC_EXPECT_TRUE(transcript.find("RELOCATE_LIVE_RESULT") != std::string::npos);
    }
}
}
GC_COMPONENT_OTHER_VM_TEST(RelocateLiveness, MutatorRejectsUnmarkedSource) { CheckRelocateLiveness(false, false); }
GC_COMPONENT_OTHER_VM_TEST(RelocateLiveness, MutatorCopiesMarkedSource) { CheckRelocateLiveness(false, true); }
GC_COMPONENT_OTHER_VM_TEST(RelocateLiveness, WorkerCopiesMarkedSource) { CheckRelocateLiveness(true, true); }
#endif

// ZGC zRelocate.cpp:354-380,801-835: mutator publishes a copy; only the
// worker remembers promoted fields. Observe the product bitmap, not a counter.
#include "Heap/z/zBarrier.hpp"
static void CheckRelocationRemsetOwnership(bool worker)
{
    CreateStandaloneHeap(16);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& young = heap.young();
    young.InitializeWorkers(1);
    young.Workers()->set_active_workers(1);
    GenerationSequenceFixture::Advance(young);
    ZGenerationTest::SetTenuringThreshold(young, 1);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetFlagHasRefField();
    type->SetInstanceSize(sizeof(uintptr_t));
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT | 1;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    ZPage* source = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::survivor1, flags);
    ZPage* childPage = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::eden, flags);
    GC_EXPECT_TRUE(source != nullptr && childPage != nullptr);
    auto* object = reinterpret_cast<BaseObject*>(source->alloc_object(16));
    auto* child = reinterpret_cast<BaseObject*>(childPage->alloc_object(16));
    object->SetClassInfo(type);
    child->SetClassInfo(type);
    HeapSlotAt<>(reinterpret_cast<MAddress>(child) + 8).StoreColoured(StoreGoodPointer(nullptr));
    HeapSlotAt<>(reinterpret_cast<MAddress>(object) + 8).StoreColoured(StoreGoodPointer(child));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(source, object));
    ZPageTest::MakeRelocatable(*source);
    ZRelocationSetSelector selector(0.0);
    selector.register_live_page(source);
    // The selector needs two sparse pages to reclaim a whole page.
    ZPage* peer = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, PageAge::survivor1, flags);
    GC_EXPECT_TRUE(peer != nullptr);
    auto* peerObject = reinterpret_cast<BaseObject*>(peer->alloc_object(16));
    peerObject->SetClassInfo(type);
    HeapSlotAt<>(reinterpret_cast<MAddress>(peerObject) + 8).StoreColoured(StoreGoodPointer(child));
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(peer, peerObject));
    ZPageTest::MakeRelocatable(*peer);
    selector.register_live_page(peer);
    selector.select();
    young.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&young.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { young.forwarding_table().insert(owner); }
    auto* forwarding = forwarding_for_page(source);
    GC_EXPECT_TRUE(forwarding != nullptr && forwarding->to_age() == PageAge::old);
    zpointer root = StoreGoodPointer(object);
    ZGlobalsPointers::flip_young_relocate_start();
    young.set_phase(ZGenerationPhase::Relocate);
    BaseObject* result;
    if (worker) {
        young.relocate().relocate(&young.relocation_set());
        result = reinterpret_cast<BaseObject*>(forwarding->find(reinterpret_cast<MAddress>(object)));
    } else {
        result = to_object(ZBarrier::load_barrier_on_oop_field(&root));
    }
    GC_EXPECT_TRUE(result != nullptr && result != object);
    auto* target = Heap::page(reinterpret_cast<MAddress>(result));
    auto* field = reinterpret_cast<volatile zpointer*>(reinterpret_cast<MAddress>(result) + 8);
    const bool remembered = target->is_remembered(field);
    const bool copied = to_object(RefField<>(*field).GetTargetObject()) == child;
    const bool published = forwarding->find(reinterpret_cast<MAddress>(object)) == reinterpret_cast<MAddress>(result);
    std::fprintf(stderr, "RELOCATE958_RESULT worker=%d remembered=%d copied=%d published=%d old=%d target_assertion=executed\n",
                 worker, remembered, copied, published, target->age() == PageAge::old);
    GC_EXPECT_EQ(remembered, worker);
    GC_EXPECT_TRUE(copied && published && target->age() == PageAge::old);
}
GC_COMPONENT_OTHER_VM_TEST(RelocateInner958, MutatorDoesNotRememberPromotion)
{
    CheckRelocationRemsetOwnership(false);
}
GC_COMPONENT_OTHER_VM_TEST(RelocateInner958, WorkerRemembersPromotion)
{
    CheckRelocationRemsetOwnership(true);
}

#include "Heap/z/zObjectAllocator.hpp"
GC_COMPONENT_OTHER_VM_TEST(RelocateInner958, WorkerWinnerUndoesMutatorAllocation)
{
    CreateStandaloneHeap(64);
    ZHeuristics::set_max_heap_size(128 * 1024 * 1024);
    ZHeuristics::set_medium_page_size();
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& generation = heap.old();
    generation.InitializeWorkers(1);
    generation.Workers()->set_active_workers(1);
    GenerationSequenceFixture::Advance(generation);
    const size_t size = ZObjectSizeLimitSmall + ZObjectAlignmentMedium;
    GC_EXPECT_TRUE(size <= ZObjectSizeLimitMedium);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(size - sizeof(uintptr_t));
    type->SetAlign(8);
    GCTib tib{};
    tib.tag = SIGN_BIT;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    ZAllocationFlags flags;
    flags.set_non_blocking();
    BaseObject* objects[2];
    ZPage* pages[2];
    ZRelocationSetSelector selector(0.0);
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(ZPageSizeMediumMax, ZPageType::medium, false, PageAge::old, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        objects[i] = reinterpret_cast<BaseObject*>(pages[i]->alloc_object(size));
        objects[i]->SetClassInfo(type);
        *reinterpret_cast<uint64_t*>(reinterpret_cast<MAddress>(objects[i]) + 8) = 0x958;
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], objects[i]));
        ZPageTest::MakeRelocatable(*pages[i]);
        selector.register_live_page(pages[i]);
    }
    selector.select();
    generation.relocation_set().install(&selector);
    ZRelocationSetIterator installed(&generation.relocation_set());
    for (ZForwarding* owner; installed.next(&owner);) { generation.forwarding_table().insert(owner); }
    ZForwarding* owner = forwarding_for_page(pages[0]);
    GC_EXPECT_TRUE(owner != nullptr);
    zpointer root = StoreGoodPointer(objects[0]);
    ZGlobalsPointers::flip_old_relocate_start();
    generation.set_phase(ZGenerationPhase::Relocate);
    auto* allocator = heap.object_allocator().allocator(PageAge::old);
    GC_EXPECT_TRUE(*allocator->shared_medium_page_addr() == nullptr);
    // Hold the existing product lock, so a retained mutator cannot allocate
    // until the real worker has inserted the winning forwarding entry.
    std::unique_lock<ZLock> lock(allocator->mediumPageAllocLock);
    BaseObject* result = nullptr;
    std::thread mutator([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        result = to_object(ZBarrier::load_barrier_on_oop_field(&root));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (owner->ref_count().load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    const bool retained = owner->ref_count().load(std::memory_order_acquire) == 2;
    std::thread worker([&] { generation.relocate().relocate(&generation.relocation_set()); });
    const MAddress from = reinterpret_cast<MAddress>(objects[0]);
    const auto publishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    MAddress winner = 0;
    while ((winner = owner->find(from)) == 0 &&
           std::chrono::steady_clock::now() < publishDeadline) { std::this_thread::yield(); }
    lock.unlock();
    mutator.join();
    worker.join();
    ZPage* unused = *allocator->shared_medium_page_addr();
    const size_t allocated = unused == nullptr ? size : unused->GetRegionAllocatedSize();
    const bool published = winner != 0 && reinterpret_cast<MAddress>(result) == winner;
    const bool copied = result != nullptr && *reinterpret_cast<uint64_t*>(reinterpret_cast<MAddress>(result) + 8) == 0x958;
    std::fprintf(stderr, "RELOCATE958_UNDO retained=%d winner=%zx result=%p copied=%d unused=%p allocated=%zu target_assertion=executed\n",
                 retained, winner, result, copied, unused, allocated);
    // Keep the result assertion independent of the scheduling preconditions.
    GC_EXPECT_EQ(allocated, 0u);
    GC_EXPECT_TRUE(retained && published && copied && unused != nullptr);
}
