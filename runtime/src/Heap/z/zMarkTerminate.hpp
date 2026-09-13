// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKTERMINATE_HPP
#define MRT_ZMARKTERMINATE_HPP
#include <cstddef>
#include <mutex>
#include <condition_variable>

namespace MapleRuntime {
class MarkStripeSet;
class MarkTerminate {
public:
    void Reset(size_t workers);
    void Leave();
    bool TryTerminate(MarkStripeSet& stripes, size_t usedNStripes);
    void Wake();
    bool Saturated() const;
    bool Terminated() const;
    size_t WorkerCount() const;

private:
    void MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes);

    size_t workerCount = 0;
    size_t working = 0;
    size_t awakening = 0;
    bool terminated = false;
    mutable std::mutex mutex;
    std::condition_variable condition;
};
} // namespace MapleRuntime
#endif
