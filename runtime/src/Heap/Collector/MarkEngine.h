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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/Collector/MarkStripe.h"
#include "Heap/Collector/ZAbort.hpp"
#include "Heap/Verify/VerifyMarkingStacks.h"

namespace MapleRuntime {

class MarkStripeSet;
class GCWorkers;

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

class MarkDomain;

class MarkEngine {
public:
    enum class Result { Completed, Partial, Aborted };

    using Process = std::function<void(const MarkStackEntry&)>;

    static Result FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                             MarkTerminate& terminate, size_t workerId, bool partial,
                             const Process& process, std::atomic<size_t>* stealSuccess = nullptr,
                             std::atomic<size_t>* stealFailure = nullptr, MarkDomain* domain = nullptr);
};

// Per-generation mark ownership (zMark.cpp:80, zGeneration.hpp:70, zThreadLocalData.hpp:43).
// zGeneration.cpp:897-915 / 1261-1271: mark_end success only sets Phase::MarkComplete.
// reset_relocation_set (zGeneration.cpp:276-285) is a later last-consumer, not this answer.
struct MarkClosure {
    VerifyMarkingStacks::MarkingGeneration generation = VerifyMarkingStacks::MarkingGeneration::YOUNG;
    uint64_t seq = 0;
    bool completed = false;
};

class MarkDomain {
public:
    explicit MarkDomain(size_t capacity, VerifyMarkingStacks::MarkingGeneration generation);
    void PrepareWork(size_t nworkers);
    void ResizeWorkers(size_t nworkers);
    void FinishWork();
    void BindWorkers(GCWorkers* workers) { gcWorkers = workers; }
    void BindAbort(ZAbort* token) { abortToken = token; }
    bool PollStop();
    MarkStripeSet& Stripes() { return stripes; }
    MarkTerminate& Terminate() { return terminate; }
    MarkingSMR& Smr() { return *smr; }
    MarkThreadLocalStacks& Stacks(size_t workerId) { return *stacks[workerId]; }
    size_t NWorkers() const { return nworkers; }
    size_t TargetNStripes() const { return targetNStripes; }
    bool FlushStacks();
    bool TryTerminateFlush();
    bool TryEnd();
    MarkClosure NoteMarkComplete();
    const MarkClosure& LastClosure() const { return lastClosure; }
    VerifyMarkingStacks::MarkingGeneration Generation() const { return generation; }

private:
    void EnsureWorkers(size_t nworkers);

    size_t nworkers = 0;
    size_t targetNStripes = 0;
    VerifyMarkingStacks::MarkingGeneration generation;
    MarkStripeSet stripes;
    MarkTerminate terminate;
    std::unique_ptr<MarkingSMR> smr;
    std::vector<std::unique_ptr<MarkThreadLocalStacks>> stacks;
    GCWorkers* gcWorkers = nullptr;
    ZAbort* abortToken = nullptr;
    uint64_t closureSeq = 0;
    MarkClosure lastClosure;
};

} // namespace MapleRuntime

#endif
