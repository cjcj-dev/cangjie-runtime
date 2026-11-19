// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_JNI_CRITICAL_HPP
#define MRT_Z_JNI_CRITICAL_HPP

#include <atomic>
#include <cstdint>
#include "Heap/z/zLock.hpp"

namespace MapleRuntime {

// ZJNICritical (zJNICritical.hpp:33-50; zJNICritical.cpp:51-181): process-wide
// count + condition lock. count>=0 enter allowed; -1 blocked; < -1 block in progress.
class ZJNICritical {
public:
    static void initialize();
    // Bounded, shareable variant used by the collector. Waits at most `millis`
    // for current holders to leave. On success the caller owns (or shares) the
    // block and must call unblock_bounded(); on timeout the count is restored,
    // waiting acquirers are released, and false is returned.
    static bool block_bounded(uint64_t millis);
    static void unblock_bounded();
    static void enter();
    static void exit();
    static int64_t count_snapshot() { return count.load(std::memory_order_acquire); }

private:
    static void enter_inner();
    static void exit_inner();

    static std::atomic<int64_t> count;
    static ZConditionLock* lock;
    // Collector parties sharing a drained (-1) bounded block; guarded by lock.
    static int64_t boundedBlockers;
    // Set when a bounded block timed out on holders that are still inside;
    // cleared when the count next drains to 0 (or -1). While set and holders
    // remain, block_bounded() reports the timeout at once instead of holding the
    // count negative, so other acquirers are not stalled for another bound.
    static std::atomic<bool> stuck;
};

} // namespace MapleRuntime

#endif
