// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Phase-unit input, not a complete driver/managed workload.
#include "gc_verify_fixture.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zWorkers.hpp"
#include <cstdio>
#include <cstring>
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

ZForwarding* p16_forwarding = nullptr;
volatile uintptr_t* p16_destination_field = nullptr;
uintptr_t p16_remset_mask = 0;
ZGenerationYoung* p16_young = nullptr;

int main(int argc, char** argv)
{
    if (argc != 2) { return 78; }
    const bool scan = std::strcmp(argv[1], "scan") == 0;
    const bool young = scan || std::strcmp(argv[1], "young") == 0;
    ConcGCThreads = 64;
    ZGlobalsPointers::initialize();
    ThreadLocal::InitializeCleaner();
    ZCPU::initialize();
    WorkerFixture worker;
    GcVerifyFixture fixture;
    auto& heap = Heap::GetHeap();
    heap.old().InitializeWorkers(1);
    heap.young().InitializeWorkers(1);
    fixture.PrepareOldSource();
    p16_forwarding = forwarding_for_page(fixture.region0);
    p16_young = &heap.young();
    p16_remset_mask = ZPointerRememberedMask;
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    HeapSlotAt<>(from + TYPEINFO_PTR_SIZE).StoreColoured(
        to_zpointer(raw(StoreGoodPointer(nullptr)) | ZPointerRememberedMask));
    heap.old().pause_relocate_start();
    if (young) {
        // Register before the actual mark-start flip so the real remset-table
        // iterator finds the source in the previous found-old set.
        heap.remembered().register_found_old(fixture.region0);
        ScopedStopTheWorld stopped("VerifyRemsetPhaseInput");
        heap.young().mark_start();
    }
    if (heap.young().is_phase_mark() != young || !heap.old().is_phase_relocate()) { return 79; }
    auto& manager = static_cast<RegionSpace&>(heap.GetAllocator()).GetRegionManager();
    BaseObject* destination = fixture.obj0;
    if (scan) {
        // Ordinary allocator input only. The product performs the copy and
        // publishes its own mapping, while its source still has an owner.
        fixture.region1->reset(PageAge::old);
        fixture.region1->SetRegionAllocPtr(fixture.region1->GetRegionStart());
        auto& allocator = *heap.object_allocator().allocator(PageAge::old);
        ZPerCPUIterator<ZPage*> slots(&allocator.sharedSmallPage);
        for (ZPage** slot; slots.next(&slot);) { __atomic_store_n(slot, fixture.region1, __ATOMIC_RELEASE); }
        destination = Heap::GetHeap().relocate_or_remap_object(fixture.obj0, ZGenerationId::old);
        if (destination == nullptr || destination == fixture.obj0) { return 80; }
    }
    p16_destination_field = reinterpret_cast<volatile uintptr_t*>(
        reinterpret_cast<MAddress>(destination) + TYPEINFO_PTR_SIZE);
    ZPage* page = Heap::page(reinterpret_cast<MAddress>(destination));
    auto* field = reinterpret_cast<volatile zpointer*>(p16_destination_field);
    if (page->is_remembered(field) || page->was_remembered(field)) { return 81; }
    std::fprintf(stderr, "VERIFY_REMSET_INPUT_READY mode=%s young_mark=%d from=%#zx destination=%p field=%p word=%#zx\n",
                 argv[1], heap.young().is_phase_mark(), from, destination, p16_destination_field,
                 static_cast<uintptr_t>(*p16_destination_field));
    if (scan) { heap.remembered().scan_and_follow(&heap.young().Mark()); }
    else { manager.ForwardFromRegions<Generation::Old>(); }
    const MAddress actual = p16_forwarding->find(from);
    const bool matched = actual == reinterpret_cast<MAddress>(destination) &&
        (*p16_destination_field & ZPointerRememberedMask) == ZPointerRememberedMask;
    std::fprintf(stderr, "VERIFY_REMSET_COMPLETION_ASSERT_EXECUTED mode=%s actual=%#zx expected=%p matched=%d\n",
                 argv[1], actual, destination, matched);
    std::fflush(nullptr);
    // This fixture owns a phase, not a runtime shutdown/allocator teardown.
    _exit(matched ? 0 : 82);
}
