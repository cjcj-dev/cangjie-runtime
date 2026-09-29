// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKTERMINATE_HPP
#define MRT_ZMARKTERMINATE_HPP
#include "Heap/z/zLock.inline.hpp"
#include <cstddef>
#include <atomic>

namespace MapleRuntime {
class MarkStripeSet;
class MarkTerminate {
    friend class ZMark;
public:
    void SetResurrected(bool value) { resurrected.store(value, std::memory_order_relaxed); }
    bool Resurrected() const { return resurrected.load(std::memory_order_relaxed); }
    void Reset(size_t workers);
    void Leave();
    bool TryTerminate(MarkStripeSet& stripes, size_t usedNStripes);
    void Wake();
    bool Saturated() const;

private:
    void MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes);

    std::atomic<bool> resurrected{false};
    size_t workerCount = 0;
    size_t working = 0;
    size_t awakening = 0;
    mutable ZConditionLock mutex;
};
} // namespace MapleRuntime
#endif
