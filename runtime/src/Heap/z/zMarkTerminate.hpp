// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKTERMINATE_HPP
#define MRT_ZMARKTERMINATE_HPP
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace MapleRuntime {
class MarkStripeSet;
class MarkTerminate {
    friend class ZMark;
public:
    void SetResurrected(bool value) { resurrected.store(value, std::memory_order_relaxed); }
    bool Resurrected() const { return resurrected.load(std::memory_order_relaxed); }
    void Reset(uint32_t workers);
    void Leave();
    bool TryTerminate(MarkStripeSet& stripes, size_t usedNStripes);
    void Wake();
    bool Saturated() const;

private:
    void MaybeReduceStripes(MarkStripeSet& stripes, size_t usedNStripes);

    uint32_t workerCount = 0;
    std::atomic<uint32_t> working{0};
    std::atomic<uint32_t> awakening{0};
    std::atomic<bool> resurrected{false};
    mutable std::mutex mutex;
    std::condition_variable condition;
};
} // namespace MapleRuntime
#endif
