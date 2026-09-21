// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
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
GC_COMPONENT_OTHER_VM_TEST(RelocationTargets, InPlaceTargetReusedAndSourceFreed)
{
    CreateStandaloneHeap(2);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    auto& heap = Heap::GetHeap();
    auto& manager = heap.page_allocator();
    auto& old = heap.old();
    old.InitializeWorkers(1);
    old.Workers()->set_active_workers(1);
    old.Begin(1);
    GenerationSequenceFixture::Advance(old);

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
    MAddress starts[2];
    MAddress objects[2];
    for (size_t i = 0; i < 2; ++i) {
        pages[i] = Heap::alloc_page(ZPageSizeSmall, ZPageType::small, false, false, true, PageAge::old, flags);
        GC_EXPECT_TRUE(pages[i] != nullptr);
        starts[i] = pages[i]->GetRegionStart();
        objects[i] = pages[i]->alloc_object(24);
        auto* dead = reinterpret_cast<BaseObject*>(objects[i]);
        dead->SetClassInfo(type);
        objects[i] = pages[i]->alloc_object(24);
        auto* object = reinterpret_cast<BaseObject*>(objects[i]);
        object->SetClassInfo(type);
        *reinterpret_cast<uint64_t*>(objects[i] + 8) = 0x796000 + i;
        GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(pages[i], object));
    }
    GC_EXPECT_EQ(manager.GetUsedBytes(), 2 * ZPageSizeSmall);
    GC_EXPECT_TRUE(BeginForwardingArena(Generation::Old, {pages[0], pages[1]}));
    ZForwarding* owners[2] = {forwarding_for_page(pages[0]), forwarding_for_page(pages[1])};
    old.set_phase(ZGenerationPhase::Relocate);
    old.relocate().relocate(&old.relocation_set());
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
    GC_EXPECT_EQ(target0->GetRegionAllocatedSize(), 48u);
    GC_EXPECT_EQ(retired, 1u);
    GC_EXPECT_EQ(manager.GetUsedBytes(), ZPageSizeSmall);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destinations[0] + 8), 0x796000u);
    GC_EXPECT_EQ(*reinterpret_cast<uint64_t*>(destinations[1] + 8), 0x796001u);
}
