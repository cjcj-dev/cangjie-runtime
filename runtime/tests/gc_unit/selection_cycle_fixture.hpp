#include "gc_allocation_flags.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once

#include "gc_heap_fixture.hpp"

namespace MapleRuntime::GcUnit {
// Full selection may free any empty page (ZGC zGeneration.cpp:169-176).
// These five phase tests therefore obtain backing from the product allocator,
// while retaining their directed object graphs and existing assertions.
struct SelectionCycleFixture {
    explicit SelectionCycleFixture(PageAge secondAge = PageAge::old)
    {
        EnsureZAddressDomain();
        ZStat::Initialize();
        // RunAll creates the standalone heap before this fixture. Heap's
        // constructor initializes its allocator and injects it into young's
        // remembered set (ZGC zHeap.cpp:58-79, zGeneration.cpp:499-505).
        ZPage* page0 = Heap::alloc_page(ZGranuleSize, ZPageType::small, false, PageAge::eden, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
        ZPage* page1 = Heap::alloc_page(ZGranuleSize, ZPageType::small, false, secondAge, MapleRuntime::GcUnit::NonBlockingAllocationFlags());
        GC_EXPECT_TRUE(page0 != nullptr && page1 != nullptr);
        starts[0] = page0->GetRegionStart();
        starts[1] = page1->GetRegionStart();
        GcHeapFixture::AdvanceGeneration(Generation::Old);
        GcHeapFixture::AdvanceGeneration(Generation::Young);
        InitFwdTables();

        typeInfo = reinterpret_cast<TypeInfo*>(typeInfoStorage);
        typeInfo->SetType(TypeKind::TYPE_KIND_CLASS);
        typeInfo->SetFlagHasRefField();
        typeInfo->SetInstanceSize(sizeof(void*));
        GCTib gctib{};
        gctib.tag = SIGN_BIT | 1;
        typeInfo->SetGCTib(gctib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(typeInfoStorage), sizeof(typeInfoStorage));
        obj0 = PlaceObject(starts[0] + 64);
        obj1 = PlaceObject(starts[1] + 64);
        region0()->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + 64);
        region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + 64);
    }

    ~SelectionCycleFixture()
    {
        ZGeneration::young()->reset_relocation_set();
        ZGeneration::old()->reset_relocation_set();
        for (const MAddress start : starts) {
            // The cycle may already have destroyed an empty page descriptor.
            if (ZPage* page = Heap::page(start)) {
                Heap::free_page(page);
            }
        }
    }

    BaseObject* PlaceObject(MAddress address)
    {
        auto* object = reinterpret_cast<BaseObject*>(address);
        *reinterpret_cast<uintptr_t*>(object) = reinterpret_cast<uintptr_t>(typeInfo);
        return object;
    }

    // Same split as GcHeapFixture: alloc_page already inserted the descriptor
    // (zHeap.cpp:253-257). Later reads go through Heap::page (zHeap.inline.hpp:60).
    ZPage* region0() const { return Heap::page(starts[0]); }
    ZPage* region1() const { return Heap::page(starts[1]); }
    BaseObject* obj0 = nullptr;
    BaseObject* obj1 = nullptr;
    TypeInfo* typeInfo = nullptr;
private:
    MAddress starts[2]{};
    alignas(TypeInfo) unsigned char typeInfoStorage[sizeof(TypeInfo)]{};
};
}
