// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#ifndef MRT_GC_VERIFY_FIXTURE_HPP
#define MRT_GC_VERIFY_FIXTURE_HPP

#include "gc_heap_fixture.hpp"
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zVerify.hpp"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"

extern "C" int CJ_ScheduleManagerInit();
namespace MapleRuntime::GcUnit {
class VerifyRuntime final : public Runtime {
    MutatorManager manager;
    Concurrency concurrency;
    Runtime* previous;
public:
    VerifyRuntime() : previous(runtime) {
        CHECK(CJ_ScheduleManagerInit() == 0);
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        concurrency.Init(ConcurrencyParam{1024, 64, 1});
    }
    ~VerifyRuntime() override { runtime = previous; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam{}; }
    void SetGCThreshold(uint64_t) override {}
};

// zVerify.cpp:125 / zHeap.inline.hpp:117: verified objects must belong to
// the heap. Keep this setup local to the verifier tests; the shared fixture
// deliberately does not initialize the complete runtime heap.
struct GcVerifyFixture : GcHeapFixture {
    VerifyRuntime runtime;
    GcVerifyFixture()
    {
        // Relocation preparation walks allocated objects from the page start.
        // Use a dense one-object page, without the shared fixture's empty prefix.
        obj0 = PlaceObject(region0()->GetRegionStart());
        obj1 = PlaceObject(region1()->GetRegionStart());
        region0()->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + RegionSpace::GetAllocSize(*obj0));
        region1()->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + RegionSpace::GetAllocSize(*obj1));
    }

    void VerifyRoot(BaseObject* object)
    {
        auto& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
        NativeSlot* root = storage.Allocate();
        root->StoreColoured(StoreGoodPointer(object));
        ZVerify::BeforeZOperation();
        NativeAccess<>::oop_store(root, nullptr);
        storage.Release(root);
    }

    void VerifyObject(BaseObject* object, bool weak)
    {
        auto& storage = Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage();
        NativeSlot* root = storage.Allocate();
        root->StoreColoured(StoreGoodPointer(object));
        (void)RegionSpace::MarkObject<Generation::Old>(object);
        Heap::GetHeap().GetZGeneration(Generation::Old).set_phase(ZGenerationPhase::MarkComplete);
        if (weak) { ZVerify::AfterWeakProcessing(); }
        else { ZVerify::AfterMark(); }
        NativeAccess<>::oop_store(root, nullptr);
        storage.Release(root);
    }

    void PrepareOldSource()
    {
        region0()->reset(PageAge::old);
        region1()->reset(PageAge::old);
        (void)RegionSpace::MarkObject<Generation::Old>(obj0);
        Heap::GetHeap().GetZGeneration(Generation::Old)
            .set_phase(ZGenerationPhase::MarkComplete);
        // zRelocationSet.cpp:110-118: select pages and install the arena before
        // preparing a source page or verifying its forwarding entries.
        // A second actual live page permits the selector to reclaim one page.
        (void)RegionSpace::MarkObject<Generation::Old>(obj1);
        CHECK(BeginForwardingArena(Generation::Old, {region0(), region1()}));
    }
};
} // namespace MapleRuntime::GcUnit
#endif
