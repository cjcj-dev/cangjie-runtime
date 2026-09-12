// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkEngine.h"

#include "Base/Log.h"
#include "Heap/GcThreadPool.h"
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
        (void)stripes.TrySetNStripes(nstripes, nstripes >> 1);
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
    condition.wait(lock);
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
                          MarkDomain* domain)
{
    const size_t assumed = context.NStripes();
    const size_t nstripes = stripes.NStripes();
    if (assumed != nstripes) {
        context.SetNStripes(nstripes);
    } else if (nstripes < stripes.CalculateNStripes(terminate.WorkerCount()) && stripes.IsCrowded()) {
        const size_t restored = nstripes << 1;
        if (stripes.TrySetNStripes(nstripes, restored)) {
            context.SetNStripes(restored);
        }
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
    return domain != nullptr && domain->PollStop();
}

static bool Drain(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                  size_t workerId, const MarkEngine::Process& process, MarkDomain* domain)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripeId(stripes.StripeForWorker(terminate.WorkerCount(), workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
        process(entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, stripes, terminate, workerId, domain)) {
            return false;
        }
    }
    return true;
}

MarkEngine::Result MarkEngine::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure, MarkDomain* domain)
{
    for (;;) {
        if (!Drain(context, smr, stripes, terminate, workerId, process, domain)) {
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

MarkDomain::MarkDomain(size_t capacity, VerifyMarkingStacks::MarkingGeneration generation)
    : generation(generation), stripes(capacity)
{
    stripes.SetTerminate(&terminate);
}

void MarkDomain::EnsureWorkers(size_t workers)
{
    if (smr == nullptr || smr->WorkerCount() < workers) {
        smr = std::make_unique<MarkingSMR>(workers);
    }
    while (stacks.size() < workers) {
        stacks.emplace_back(std::make_unique<MarkThreadLocalStacks>(stripes.Count()));
    }
}

void MarkDomain::PrepareWork(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = stripes.CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
    terminate.Reset(workers, generation);
}

void MarkDomain::ResizeWorkers(size_t workers)
{
    PrepareWork(workers);
}

void MarkDomain::FinishWork() {}

bool MarkDomain::PollStop()
{
    if (abortToken != nullptr && abortToken->Poll()) {
        return true;
    }
    if (gcWorkers != nullptr && gcWorkers->ShouldWorkerResize()) {
        return true;
    }
    return false;
}

bool MarkDomain::FlushStacks()
{
    bool flushed = false;
    for (auto& stack : stacks) {
        if (stack != nullptr && stack->Flush(stripes, true)) {
            flushed = true;
        }
    }
    return flushed;
}

bool MarkDomain::TryTerminateFlush()
{
    const bool flushed = FlushStacks();
    return flushed || !stripes.IsEmpty();
}

bool MarkDomain::TryEnd()
{
    (void)FlushStacks();
    return stripes.IsEmpty();
}

} // namespace MapleRuntime
