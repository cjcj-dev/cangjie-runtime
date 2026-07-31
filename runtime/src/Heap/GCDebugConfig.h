// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_DEBUG_CONFIG_H
#define MRT_GC_DEBUG_CONFIG_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class __attribute__((visibility("hidden"))) GCDebugConfig {
public:
    static constexpr uint8_t CLOBBER_PATTERN = 0xA5;

    static void ConfigureFromEnvironment();
    static void DisableStress();

    static bool IsStressEnabled()
    {
        return stressMinorInterval.load(std::memory_order_relaxed) != 0 ||
            stressMajorInterval.load(std::memory_order_relaxed) != 0;
    }

    static bool IsClobberEnabled() { return clobberEnabled.load(std::memory_order_relaxed); }
    static bool IsClobberPositiveControlEnabled()
    {
        return clobberPositiveControlEnabled.load(std::memory_order_relaxed);
    }

    static void FillReclaimedMemory(uintptr_t start, size_t size);
    static void FillYoungReclaimedMemory(uintptr_t start, size_t size, size_t allocatedSize);
    static void FillFreePinnedPayload(uintptr_t start, size_t size);

    static bool ShouldTriggerMinor();
    static bool ShouldTriggerMajor();
    static void NoteStressMinorRequest();
    static void NoteStressMajorRequest();
    static void NoteStressMinorExecution(bool wasYoungCollection);
    static void NoteStressMajorExecution();

private:
    static std::atomic<bool> clobberEnabled;
    static std::atomic<bool> clobberPositiveControlEnabled;
    static std::atomic<bool> clobberPositiveControlRan;
    static std::atomic<size_t> stressMinorInterval;
    static std::atomic<size_t> stressMajorInterval;
    static std::atomic<size_t> stressMinorAllocationCount;
    static std::atomic<size_t> stressMajorAllocationCount;
    static std::atomic<size_t> stressMinorRequestCount;
    static std::atomic<size_t> stressMajorRequestCount;
    static std::atomic<size_t> stressMinorExecutionCount;
    static std::atomic<size_t> stressMinorYoungExecutionCount;
    static std::atomic<size_t> stressMajorExecutionCount;
};
} // namespace MapleRuntime

#endif // MRT_GC_DEBUG_CONFIG_H
