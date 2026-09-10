// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/VerifyMarkingStacks.h"

#include <atomic>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/Verify/VerifyPhase.h"

namespace MapleRuntime {
namespace VerifyMarkingStacks {
namespace {

struct ReceiptState {
    std::atomic<uint64_t>
        boundaryReceipts[MARKING_GENERATION_COUNT][MARKING_BOUNDARY_COUNT][MARKING_CONTAINER_COUNT]{};
    std::atomic<size_t> producerMax[MARKING_GENERATION_COUNT][MARKING_CONTAINER_COUNT]{};
};

ReceiptState g_receipts;

const char* GenerationName(MarkingGeneration generation)
{
    return generation == MarkingGeneration::MAJOR ? "major" : "young";
}

const char* BoundaryName(MarkingBoundary boundary)
{
    switch (boundary) {
        case MarkingBoundary::START: return "start";
        case MarkingBoundary::SEED_PUBLISH: return "seed-publish";
        case MarkingBoundary::TASK_EXIT: return "task-exit";
        case MarkingBoundary::TERMINATION: return "termination";
        case MarkingBoundary::WORKER_EXIT: return "worker-exit";
        case MarkingBoundary::JOIN: return "join";
        case MarkingBoundary::END: return "end";
        default: return "unknown";
    }
}

const char* ContainerName(MarkingContainer container)
{
    switch (container) {
        case MarkingContainer::OWNER: return "owner";
        case MarkingContainer::FOREIGN: return "foreign";
        case MarkingContainer::TASK: return "task";
        case MarkingContainer::POOL: return "pool";
        case MarkingContainer::LOCAL: return "local";
        case MarkingContainer::STRIPE: return "stripe";
        default: return "unknown";
    }
}

void RaiseMax(std::atomic<size_t>& target, size_t value)
{
    size_t observed = target.load(std::memory_order_relaxed);
    while (observed < value &&
           !target.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

void NoteBoundary(MarkingGeneration generation, MarkingBoundary boundary, MarkingContainer container)
{
    g_receipts.boundaryReceipts[static_cast<size_t>(generation)][static_cast<size_t>(boundary)]
                                        [static_cast<size_t>(container)]
        .fetch_add(1, std::memory_order_relaxed);
}

long long PrintableIndex(size_t value)
{
    return value == NO_MARKING_INDEX ? -1 : static_cast<long long>(value);
}

} // namespace

bool Enabled()
{
    return VerifyFaceEnabled(VerifyFace::Marking);
}

void NoteProducer(MarkingGeneration generation, MarkingContainer container, size_t pending)
{
    if (!Enabled()) {
        return;
    }
    RaiseMax(g_receipts.producerMax[static_cast<size_t>(generation)][static_cast<size_t>(container)], pending);
}

void VerifyEmpty(MarkingGeneration generation, MarkingBoundary boundary, MarkingContainer container,
                 size_t pending, size_t owner, size_t worker, size_t stripe)
{
    if (!VerifyPhaseEnter(VerifyFace::Marking, BoundaryName(boundary))) {
        return;
    }
    NoteBoundary(generation, boundary, container);
    CHECK_DETAIL(pending == 0,
                 "[GCV2][marking-stack] generation=%s boundary=%s container=%s owner=%lld worker=%lld "
                 "stripe=%lld pending=%zu",
                 GenerationName(generation), BoundaryName(boundary), ContainerName(container),
                 PrintableIndex(owner), PrintableIndex(worker), PrintableIndex(stripe), pending);
    VLOG(REPORT,
         "[GCV2][marking-stack] PASS generation=%s boundary=%s container=%s owner=%lld worker=%lld "
         "stripe=%lld pending=%zu",
         GenerationName(generation), BoundaryName(boundary), ContainerName(container),
         PrintableIndex(owner), PrintableIndex(worker), PrintableIndex(stripe), pending);
}

#if defined(MRT_TESTABLE_INTERNALS)
Snapshot ReadSnapshot()
{
    Snapshot snapshot;
    for (size_t generation = 0; generation < MARKING_GENERATION_COUNT; ++generation) {
        for (size_t boundary = 0; boundary < MARKING_BOUNDARY_COUNT; ++boundary) {
            for (size_t container = 0; container < MARKING_CONTAINER_COUNT; ++container) {
                snapshot.boundaryReceipts[generation][boundary][container] =
                    g_receipts.boundaryReceipts[generation][boundary][container].load(std::memory_order_relaxed);
            }
        }
        for (size_t container = 0; container < MARKING_CONTAINER_COUNT; ++container) {
            snapshot.producerMax[generation][container] =
                g_receipts.producerMax[generation][container].load(std::memory_order_relaxed);
        }
    }
    return snapshot;
}
#endif

} // namespace VerifyMarkingStacks
} // namespace MapleRuntime
