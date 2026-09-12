// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_ENGINE_H
#define MRT_MARK_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/Collector/MarkStripe.h"
#include "Heap/Verify/VerifyMarkingStacks.h"

namespace MapleRuntime {

class MarkStripeSet;

// ZGC zMarkTerminate.inline.hpp:43-125.
class MarkTerminate {
public:
    void Reset(size_t workers,
               VerifyMarkingStacks::MarkingGeneration generation = VerifyMarkingStacks::MarkingGeneration::YOUNG);
    void Leave();
    bool TryTerminate(MarkStripeSet& stripes, size_t usedNStripes);
    void Wake();
    bool Saturated() const;
    bool Terminated() const;
    size_t WorkerCount() const;

private:
    void MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes);

    VerifyMarkingStacks::MarkingGeneration generation = VerifyMarkingStacks::MarkingGeneration::YOUNG;
    size_t workerCount = 0;
    size_t working = 0;
    size_t awakening = 0;
    bool terminated = false;
    mutable std::mutex mutex;
    std::condition_variable condition;
};

class MarkEngine {
public:
    enum class Result { Completed, Partial, Aborted };

    using Process = std::function<void(const MarkStackEntry&)>;

    static Result FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                             MarkTerminate& terminate, size_t workerId, bool partial,
                             const Process& process, std::atomic<size_t>* stealSuccess = nullptr,
                             std::atomic<size_t>* stealFailure = nullptr, std::atomic<bool>* abort = nullptr);
};

// ZGC ZMark per generation: prepare_work / resize_workers / finish_work
// (zMark.cpp:142-163, :925-930) plus restartable task (zMark.cpp:895-923).
class MarkWork {
public:
    static constexpr size_t STRIPES_MAX = 16;

    static size_t CalculateNStripes(size_t nworkers);

    void Prepare(size_t nworkers, VerifyMarkingStacks::MarkingGeneration generation);
    void ResizeWorkers(size_t nworkers);
    void Finish();

    size_t NWorkers() const { return nworkers; }
    MarkStripeSet& Stripes() { return *stripes; }
    MarkingSMR& Smr() { return *smr; }
    MarkTerminate& Terminate() { return terminate; }
    MarkThreadLocalStacks& StacksFor(size_t workerId) { return *stacks[workerId]; }
    std::atomic<bool>& AbortFlag() { return abort; }

private:
    void EnsureWorkers(size_t nworkers);

    VerifyMarkingStacks::MarkingGeneration generation = VerifyMarkingStacks::MarkingGeneration::YOUNG;
    size_t nworkers = 0;
    std::unique_ptr<MarkStripeSet> stripes;
    std::unique_ptr<MarkingSMR> smr;
    std::vector<std::unique_ptr<MarkThreadLocalStacks>> stacks;
    MarkTerminate terminate;
    std::atomic<bool> abort{ false };
};

} // namespace MapleRuntime

#endif
