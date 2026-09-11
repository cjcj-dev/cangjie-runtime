// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkEngine.h"

#include "Base/Log.h"
#include "Heap/Verify/VerifyMarkingStacks.h"

namespace MapleRuntime {

void MarkTerminate::Reset(size_t workers, VerifyMarkingStacks::MarkingGeneration gen)
{
    CHECK_DETAIL(workers != 0, "mark termination needs a worker");
    std::lock_guard<std::mutex> lock(mutex);
    generation = gen;
    workerCount = workers;
    working = workers;
    awakening = 0;
    terminated = false;
}

bool MarkTerminate::TryTerminate(const MarkStripeSet& stripes)
{
    std::unique_lock<std::mutex> lock(mutex);
    CHECK_DETAIL(working != 0, "mark worker left termination twice");
    --working;
    if (working == 0 && stripes.IsEmpty()) {
        VerifyMarkingStacks::VerifyEmpty(generation, VerifyMarkingStacks::MarkingBoundary::TERMINATION,
                                         VerifyMarkingStacks::MarkingContainer::STRIPE, stripes.Population(),
                                         VerifyMarkingStacks::NO_MARKING_INDEX,
                                         VerifyMarkingStacks::NO_MARKING_INDEX, stripes.FirstNonEmptyStripe());
        terminated = true;
        condition.notify_all();
        return true;
    }
    if (!stripes.IsEmpty()) {
        ++working;
        return false;
    }
    condition.wait(lock, [this]() { return terminated || awakening != 0; });
    if (terminated) {
        return true;
    }
    --awakening;
    ++working;
    return false;
}

void MarkTerminate::Wake()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (terminated || working == 0 || working + awakening == workerCount) {
        return;
    }
    ++awakening;
    condition.notify_one();
}

bool MarkTerminate::Saturated() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return terminated && working == 0;
}

size_t MarkTerminate::WorkerCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return workerCount;
}

static bool StealLocalRound(MarkContext& context, MarkStripeSet& stripes)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    const size_t home = context.StripeId();
    const size_t n = stripes.Count();
    for (size_t i = 1; i < n; ++i) {
        const size_t victim = stripes.Next(home, i);
        MarkStripeStack* stack = stacks.StealLocal(victim);
        if (stack != nullptr) {
            stacks.Install(home, stack);
            return true;
        }
    }
    return false;
}

static bool StealGlobalRound(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, size_t workerId,
                             std::atomic<size_t>* stealSuccess, std::atomic<size_t>* stealFailure)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    const size_t home = context.StripeId();
    const size_t n = stripes.Count();
    for (size_t i = 0; i < n; ++i) {
        const size_t victim = stripes.Next(home, i);
        MarkStripeStack* stack = stripes.At(victim).StealStack(smr, workerId);
        if (stack != nullptr) {
            if (stealSuccess != nullptr) {
                stealSuccess->fetch_add(1, std::memory_order_relaxed);
            }
            stacks.Install(home, stack);
            return true;
        }
        if (stealFailure != nullptr) {
            stealFailure->fetch_add(1, std::memory_order_relaxed);
        }
    }
    return false;
}

MarkEngine::Result MarkEngine::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure)
{
    for (;;) {
        MarkStackEntry entry;
        if (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
            process(entry);
            continue;
        }
        if (StealLocalRound(context, stripes)) {
            continue;
        }
        if (StealGlobalRound(context, smr, stripes, workerId, stealSuccess, stealFailure)) {
            continue;
        }
        if (context.Stacks().Flush(stripes, false)) {
            terminate.Wake();
            continue;
        }
        if (partial) {
            return Result::Partial;
        }
        if (terminate.TryTerminate(stripes)) {
            context.Cache().Flush();
            smr.Reclaim(workerId);
            return Result::Completed;
        }
    }
}

} // namespace MapleRuntime
