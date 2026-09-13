// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_UNCOMMITTER_H
#define MRT_UNCOMMITTER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Base/Globals.h"

namespace MapleRuntime {
class Allocator;

class Uncommitter {
public:
    explicit Uncommitter(Allocator& partition) : partition(partition) {}
    ~Uncommitter() { Stop(); }
    void Start();
    void Stop();
    static constexpr uint64_t kDefaultDelayNs = 300ULL * SECOND_TO_NANO_SECOND;
    static constexpr size_t kMaxUncommitChunk = 256 * MB;

    static uint64_t DelayNs();
    static bool Enabled() { return DelayNs() > 0; }

    static size_t ChunkLimit(size_t maxCapacity);
    static size_t MinCapacity(size_t liveBytes, size_t youngReserve);

    static uint64_t ParseDelayNs(const char* env);

    // The allocation producer already holds the partition page allocator lock.
    static void CancelCycleLocked();

private:
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct UncommitterTestAccess;
#endif
    static Uncommitter& Current();
    void Run();
    bool WaitUntil(uint64_t deadline);
    bool Activate();
    size_t Uncommit();
    void RegisterUncommit(size_t size);
    void RunCycle();
    void Cancel();

    // The current allocator has one logical partition (id 0).
    Allocator& partition;
    std::thread worker;
    std::mutex lock;
    std::condition_variable condition;
    std::atomic<bool> stopped{false};
    // Cycle state is protected by the partition page allocator lock.
    // Only the worker consumes progress between chunks.
    bool canceled = false;
    uint64_t cancelTime = 0;
    uint64_t cycleStart = 0;
    uint64_t nextUncommitNs = 0;
    size_t toUncommit = 0;
    size_t uncommitted = 0; // cycle progress, not a second capacity account
};
} // namespace MapleRuntime
#endif // MRT_UNCOMMITTER_H
