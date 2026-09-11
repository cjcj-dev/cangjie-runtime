// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include "Cangjie.h"
#include "Heap/Heap.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Mutator/SatbBuffer.h"

using namespace MapleRuntime;
namespace {
unsigned failures = 0;
void Expect(bool result, const char* name)
{
    std::printf("ASSERT %s %s\n", name, result ? "PASS" : "FAIL");
    std::fflush(stdout);
    failures += !result;
}
bool Same(const GCCycleSnapshot& a, const GCCycleSnapshot& b)
{
    return a.generation == b.generation && a.sequence == b.sequence &&
        a.requestIndex == b.requestIndex && a.reason == b.reason &&
        a.phase == b.phase && a.active == b.active;
}
void* Exercise(void*)
{
    Collector& collector = Heap::GetHeap().GetCollector();
    auto y0 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o0 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    collector.RequestGC(GC_REASON_USER, false);
    auto y1 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o1 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y1.sequence == y0.sequence + 1, "major_prelude_young_sequence");
    Expect(o1.sequence == o0.sequence + 1, "major_old_sequence");
    Expect(o1.reason == GC_REASON_USER && !o1.active, "major_reason_completion");
    Expect(SatbBuffer::Instance().GetGeneration() == GCCycleGeneration::OLD, "major_satb_owner");
    collector.RequestGC(GC_REASON_YOUNG, false);
    auto y2 = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG);
    auto o2 = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    Expect(y2.sequence == y1.sequence + 1, "minor_sequence");
    Expect(Same(o1, o2), "minor_preserves_old_state");
    Expect(y2.reason == GC_REASON_YOUNG && !y2.active, "minor_reason_completion");
    Expect(y2.phase == GC_PHASE_RECLAIM_SATB_NODE, "minor_phase_consumer");
    Expect(SatbBuffer::Instance().GetGeneration() == GCCycleGeneration::YOUNG, "minor_satb_owner");
    std::printf("PRODUCT_STATE young_seq=%llu old_seq=%llu young_phase=%u old_phase=%u\n",
        (unsigned long long)y2.sequence, (unsigned long long)o2.sequence,
        (unsigned)y2.phase, (unsigned)o2.phase);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(failures));
}
}
// Called from a compiler-built managed frame so collection sees real stack maps.
extern "C" int cycleExercise()
{
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&InitCJRuntime), &info)) {
        std::printf("PRODUCT_LOADED %s\n", info.dli_fname);
    }
    return static_cast<int>(reinterpret_cast<uintptr_t>(Exercise(nullptr)));
}
