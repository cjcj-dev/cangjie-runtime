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
    std::atomic<uint64_t> majorStart{ 0 };
    std::atomic<uint64_t> majorTaskExit{ 0 };
    std::atomic<uint64_t> majorTermination{ 0 };
    std::atomic<uint64_t> majorJoin{ 0 };
    std::atomic<uint64_t> majorEnd{ 0 };
    std::atomic<uint64_t> youngStart{ 0 };
    std::atomic<uint64_t> youngSeedPublish{ 0 };
    std::atomic<uint64_t> youngTaskExit{ 0 };
    std::atomic<uint64_t> youngTermination{ 0 };
    std::atomic<uint64_t> youngWorkerExit{ 0 };
    std::atomic<uint64_t> youngJoin{ 0 };
    std::atomic<uint64_t> youngEnd{ 0 };
    std::atomic<size_t> majorOwnerProducerMax{ 0 };
    std::atomic<size_t> majorTaskProducerMax{ 0 };
    std::atomic<size_t> youngOwnerProducerMax{ 0 };
    std::atomic<size_t> youngTaskProducerMax{ 0 };
    std::atomic<size_t> youngLocalProducerMax{ 0 };
    std::atomic<size_t> youngStripeProducerMax{ 0 };
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

void NoteBoundary(MarkingGeneration generation, MarkingBoundary boundary)
{
    std::atomic<uint64_t>* counter = nullptr;
    if (generation == MarkingGeneration::MAJOR) {
        switch (boundary) {
            case MarkingBoundary::START: counter = &g_receipts.majorStart; break;
            case MarkingBoundary::TASK_EXIT: counter = &g_receipts.majorTaskExit; break;
            case MarkingBoundary::TERMINATION: counter = &g_receipts.majorTermination; break;
            case MarkingBoundary::JOIN: counter = &g_receipts.majorJoin; break;
            case MarkingBoundary::END: counter = &g_receipts.majorEnd; break;
            default: break;
        }
    } else {
        switch (boundary) {
            case MarkingBoundary::START: counter = &g_receipts.youngStart; break;
            case MarkingBoundary::SEED_PUBLISH: counter = &g_receipts.youngSeedPublish; break;
            case MarkingBoundary::TASK_EXIT: counter = &g_receipts.youngTaskExit; break;
            case MarkingBoundary::TERMINATION: counter = &g_receipts.youngTermination; break;
            case MarkingBoundary::WORKER_EXIT: counter = &g_receipts.youngWorkerExit; break;
            case MarkingBoundary::JOIN: counter = &g_receipts.youngJoin; break;
            case MarkingBoundary::END: counter = &g_receipts.youngEnd; break;
            default: break;
        }
    }
    if (counter != nullptr) {
        counter->fetch_add(1, std::memory_order_relaxed);
    }
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
    if (generation == MarkingGeneration::MAJOR) {
        if (container == MarkingContainer::OWNER || container == MarkingContainer::FOREIGN) {
            RaiseMax(g_receipts.majorOwnerProducerMax, pending);
        } else if (container == MarkingContainer::TASK) {
            RaiseMax(g_receipts.majorTaskProducerMax, pending);
        }
        return;
    }
    if (container == MarkingContainer::OWNER) {
        RaiseMax(g_receipts.youngOwnerProducerMax, pending);
    } else if (container == MarkingContainer::TASK) {
        RaiseMax(g_receipts.youngTaskProducerMax, pending);
    } else if (container == MarkingContainer::LOCAL) {
        RaiseMax(g_receipts.youngLocalProducerMax, pending);
    } else if (container == MarkingContainer::STRIPE) {
        RaiseMax(g_receipts.youngStripeProducerMax, pending);
    }
}

void VerifyEmpty(MarkingGeneration generation, MarkingBoundary boundary, MarkingContainer container,
                 size_t pending, size_t owner, size_t worker, size_t stripe)
{
    if (!VerifyPhaseEnter(VerifyFace::Marking, BoundaryName(boundary))) {
        return;
    }
    NoteBoundary(generation, boundary);
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

Snapshot ReadSnapshot()
{
    return { g_receipts.majorStart.load(std::memory_order_relaxed),
             g_receipts.majorTaskExit.load(std::memory_order_relaxed),
             g_receipts.majorTermination.load(std::memory_order_relaxed),
             g_receipts.majorJoin.load(std::memory_order_relaxed),
             g_receipts.majorEnd.load(std::memory_order_relaxed),
             g_receipts.youngStart.load(std::memory_order_relaxed),
             g_receipts.youngSeedPublish.load(std::memory_order_relaxed),
             g_receipts.youngTaskExit.load(std::memory_order_relaxed),
             g_receipts.youngTermination.load(std::memory_order_relaxed),
             g_receipts.youngWorkerExit.load(std::memory_order_relaxed),
             g_receipts.youngJoin.load(std::memory_order_relaxed),
             g_receipts.youngEnd.load(std::memory_order_relaxed),
             g_receipts.majorOwnerProducerMax.load(std::memory_order_relaxed),
             g_receipts.majorTaskProducerMax.load(std::memory_order_relaxed),
             g_receipts.youngOwnerProducerMax.load(std::memory_order_relaxed),
             g_receipts.youngTaskProducerMax.load(std::memory_order_relaxed),
             g_receipts.youngLocalProducerMax.load(std::memory_order_relaxed),
             g_receipts.youngStripeProducerMax.load(std::memory_order_relaxed) };
}

} // namespace VerifyMarkingStacks
} // namespace MapleRuntime
