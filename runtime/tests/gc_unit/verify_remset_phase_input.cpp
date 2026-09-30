#include "gc_worker_fixture.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Phase-unit input, not a complete driver/managed workload.
#include "gc_verify_fixture.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zForwarding.inline.hpp"
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
    if (argc != 2 && argc != 3) { return 78; }
    const bool referent = argc == 3 && std::strcmp(argv[2], "referent") == 0;
    const bool scan = std::strcmp(argv[1], "scan") == 0;
    const bool young = scan || std::strcmp(argv[1], "young") == 0;
    ConcGCThreads = 64;
    ZGlobalsPointers::initialize();
    ZCPU::initialize();
    // Reserve exactly the fixture's pages: relocation must reuse its source
    // in place, while scan explicitly supplies its destination page below.
    HeapParam params{};
    params.heapSize = GcHeapFixture::kUnits * ZGranuleSize / 1024;
    params.regionSize = ZGranuleSize / 1024;
    params.exemptionThreshold = 0.8;
    ZHeuristics::set_max_heap_size(params.heapSize * 1024);
    ZCollectedHeap::create(params, 0.5);
    ThreadLocal::InitializeCleaner();
    WorkerFixture worker;
    GcVerifyFixture fixture;
    auto& heap = Heap::GetHeap();
    MapleRuntime::GcUnit::InitializeGenerationWorkers(heap.old(), 1);
    MapleRuntime::GcUnit::InitializeGenerationWorkers(heap.young(), 1);
    fixture.PrepareOldSource();
    if (referent) { fixture.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS); }
    p16_forwarding = forwarding_for_page(fixture.region0());
