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
#include <mutex>

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

} // namespace MapleRuntime

#endif
