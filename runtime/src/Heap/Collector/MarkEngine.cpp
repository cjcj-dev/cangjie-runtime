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

void MarkTerminate::Leave()
{
    std::lock_guard<std::mutex> lock(mutex);
    CHECK_DETAIL(working != 0, "mark worker left twice");
    --working;
    if (working == 0) {
        condition.notify_all();
    }
}

void MarkTerminate::MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes)
{
    const size_t nstripes = stripes.NStripes();
    if (usedNStripes == nstripes && nstripes > 1) {
        stripes.SetNStripes(nstripes >> 1);
    }
}

bool MarkTerminate::TryTerminate(MarkStripeSet& stripes, size_t usedNStripes)
{
    std::unique_lock<std::mutex> lock(mutex);
    CHECK_DETAIL(working != 0, "mark worker left termination twice");
    --working;
    if (working == 0) {
        VerifyMarkingStacks::VerifyEmpty(generation, VerifyMarkingStacks::MarkingBoundary::TERMINATION,
                                         VerifyMarkingStacks::MarkingContainer::STRIPE, stripes.Population(),
                                         VerifyMarkingStacks::NO_MARKING_INDEX,
                                         VerifyMarkingStacks::NO_MARKING_INDEX, stripes.FirstNonEmptyStripe());
        terminated = true;
        condition.notify_all();
        return true;
    }
    MaybeReduceStripes(stripes, usedNStripes);
    condition.wait(lock, [this]() { return terminated || awakening != 0; });
    if (awakening != 0) {
        --awakening;
    }
    if (working == 0) {
        return true;
    }
    ++working;
    return false;
}

void MarkTerminate::Wake()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (working == 0) {
        return;
    }
    if (working + awakening == workerCount) {
        return;
    }
    ++awakening;
    condition.notify_one();
}

bool MarkTerminate::Saturated() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return working + awakening == workerCount;
}

bool MarkTerminate::Terminated() const
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
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
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
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
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

static bool RebalanceWork(MarkContext& context, MarkStripeSet& stripes, MarkTerminate& terminate, size_t workerId,
                          std::atomic<bool>* abort)
{
    const size_t assumed = context.NStripes();
    const size_t nstripes = stripes.NStripes();
    if (assumed != nstripes) {
        context.SetNStripes(nstripes);
    }
    const size_t stripe = stripes.StripeForWorker(terminate.WorkerCount(), workerId);
    if (context.StripeId() != stripe) {
        context.SetStripeId(stripe);
        (void)context.Stacks().Flush(stripes, false);
        terminate.Wake();
    } else if (!terminate.Saturated()) {
        (void)context.Stacks().Flush(stripes, false);
        terminate.Wake();
    }
    return abort != nullptr && abort->load(std::memory_order_relaxed);
}

static bool Drain(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                  size_t workerId, const MarkEngine::Process& process, std::atomic<bool>* abort)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripeId(stripes.StripeForWorker(terminate.WorkerCount(), workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
        process(entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, stripes, terminate, workerId, abort)) {
            return false;
        }
    }
    return true;
}

MarkEngine::Result MarkEngine::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure, std::atomic<bool>* abort)
{
    for (;;) {
        if (!Drain(context, smr, stripes, terminate, workerId, process, abort)) {
            terminate.Leave();
            return Result::Aborted;
        }
        if (StealLocalRound(context, stripes) ||
            StealGlobalRound(context, smr, stripes, workerId, stealSuccess, stealFailure)) {
            continue;
        }
        if (context.Stacks().Flush(stripes, false)) {
            terminate.Wake();
            continue;
        }
        if (partial) {
            return Result::Partial;
        }
        if (terminate.TryTerminate(stripes, context.NStripes())) {
            context.Cache().Flush();
            smr.Reclaim(workerId);
            return Result::Completed;
        }
    }
}

} // namespace MapleRuntime
