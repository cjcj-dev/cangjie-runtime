// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#if defined(MRT_TESTABLE_INTERNALS)
#include "CangjieRuntime.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "marking_smr_test.hpp"
#include <cstdio>
#include <numeric>

using namespace MapleRuntime;
namespace {
void CollectAndCheck(ZGenerationId id, bool prepare)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 4;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    ConcurrentGCBreakpoints::AcquireControl();
    auto* generation = ZGeneration::generation(id);
    auto& smr = generation->Mark().Smr();
    if (prepare) MarkingSMRTest::prepare_protected_nodes(smr);
    const auto before = MarkingSMRTest::worker_pending_counts(smr);
    const size_t initial = std::accumulate(before.begin(), before.end(), size_t{0});
    std::printf("MARK_FREE_PREP generation=%s pending=%zu prepared=%d\n",
                generation->is_young() ? "young" : "old", initial, prepare);
    // This request enters the real driver's young and old collect methods.
    const bool reached = ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED");
    ConcurrentGCBreakpoints::RunToIdle();
    const auto after = MarkingSMRTest::worker_pending_counts(smr);
    MarkingSMRTest::clear_fixture_hazards(smr);
    ConcurrentGCBreakpoints::ReleaseControl();
    // Print every product result before asserting, including on the cut arm.
    for (size_t worker = 0; worker < after.size(); ++worker) {
        std::printf("MARK_FREE_TARGET generation=%s worker=%zu pending=%zu\n",
                    generation->is_young() ? "young" : "old", worker, after[worker]);
    }
    std::fflush(stdout);
    for (size_t pending : after) GC_EXPECT_EQ(pending, size_t{0});
    GC_EXPECT_TRUE(reached);
    GC_EXPECT_EQ(initial, prepare ? size_t{2} : size_t{0});
}
}
GC_RUNTIME_OTHER_VM_TEST(GenerationMarkFree, YoungCollectReclaimsProtectedNodes)
{
    CollectAndCheck(ZGenerationId::young, true);
}
GC_RUNTIME_OTHER_VM_TEST(GenerationMarkFree, OldCollectReclaimsProtectedNodes)
{
    CollectAndCheck(ZGenerationId::old, true);
}
GC_RUNTIME_OTHER_VM_TEST(GenerationMarkFree, YoungCollectEmptyControl)
{
    CollectAndCheck(ZGenerationId::young, false);
}
GC_RUNTIME_OTHER_VM_TEST(GenerationMarkFree, OldCollectEmptyControl)
{
    CollectAndCheck(ZGenerationId::old, false);
}
#endif
