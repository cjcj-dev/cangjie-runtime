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

// ZGC zMarkTerminate.inline.hpp:43-125. Last working worker may complete only
// after every stripe is empty. Saturated means terminated and working==0.
class MarkTerminate {
public:
    void Reset(size_t workers,
               VerifyMarkingStacks::MarkingGeneration generation = VerifyMarkingStacks::MarkingGeneration::YOUNG);
    bool TryTerminate(const MarkStripeSet& stripes);
    void Wake();
    bool Saturated() const;
    size_t WorkerCount() const;

private:
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
    enum class Result { Completed, Partial };

    using Process = std::function<void(const MarkStackEntry&)>;

    // zMark.cpp:439 drain, :471 rebalance omitted (no nstripes change mid-loop
    // here), :491/:511 steal local then global, :597 flush, :635 terminate.
    static Result FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                             MarkTerminate& terminate, size_t workerId, bool partial,
                             const Process& process, std::atomic<size_t>* stealSuccess = nullptr,
                             std::atomic<size_t>* stealFailure = nullptr);
};

} // namespace MapleRuntime

#endif
