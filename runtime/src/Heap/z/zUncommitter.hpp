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

#include "Base/Globals.h"
#include "Heap/z/zThread.hpp"

namespace MapleRuntime {
class Allocator;

// zUncommitter.hpp:33-71: a ZThread per partition. ZGC starts it in the
// constructor; this runtime's finalizer thread owns Start/Stop (P05), so the
// ZThread lifecycle is entered from Start and left from Stop.
class Uncommitter final : public ZThread {
public:
    explicit Uncommitter(Allocator& partition);
    ~Uncommitter() override { Stop(); }
    void Start();
    void Stop();
    void run_thread() override;
    void terminate() override;
    static constexpr uint64_t kDefaultDelayNs = 300ULL * SECOND_TO_NANO_SECOND;
    static constexpr size_t kMaxUncommitChunk = 256 * MB;

    static uint64_t DelayNs();
    // ZUncommit / ZUncommitDelay (HotSpot gc_globals.hpp flags; here the
    // cjUncommit / cjUncommitDelay environment variables, PLAN infra I15).
    // ZPhysicalMemoryManager::try_enable_uncommit clears ZUncommit when the
    // platform or the heap geometry rules uncommit out.
    static bool ZUncommit();
    static void SetZUncommit(bool enabled);
    static size_t ZUncommitDelay();
    static void SetZUncommitDelay(size_t seconds);
    static bool Enabled() { return ZUncommit() && DelayNs() > 0; }

    static size_t ChunkLimit(size_t maxCapacity);
    static size_t MinCapacity(size_t liveBytes, size_t youngReserve);

    static uint64_t ParseDelayNs(const char* env);

    // The allocation producer already holds the partition page allocator lock.
    static void CancelCycleLocked();

private:
    static Uncommitter& Current();
    bool WaitUntil(uint64_t deadline);
    bool Activate();
    size_t Uncommit();
    void RegisterUncommit(size_t size);
    void RunCycle();
    void Cancel();

    // The current allocator has one logical partition (id 0).
    Allocator& partition;
    bool started = false;
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
