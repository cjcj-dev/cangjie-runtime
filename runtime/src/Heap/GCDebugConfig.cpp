// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GCDebugConfig.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Common/BaseObject.h"

namespace MapleRuntime {
std::atomic<bool> GCDebugConfig::clobberEnabled{ false };
std::atomic<bool> GCDebugConfig::clobberPositiveControlEnabled{ false };
std::atomic<bool> GCDebugConfig::clobberPositiveControlRan{ false };
std::atomic<size_t> GCDebugConfig::stressMinorInterval{ 0 };
std::atomic<size_t> GCDebugConfig::stressMajorInterval{ 0 };
std::atomic<size_t> GCDebugConfig::stressMinorAllocationCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMajorAllocationCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMinorRequestCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMajorRequestCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMinorExecutionCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMinorYoungExecutionCount{ 0 };
std::atomic<size_t> GCDebugConfig::stressMajorExecutionCount{ 0 };

namespace {
bool ReadBoolean(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || strcmp(value, "0") == 0) {
        return false;
    }
    if (strcmp(value, "1") == 0) {
        return true;
    }
    LOG(RTLOG_ERROR, "Unsupported %s=%s; expected 0 or 1, using 0", name, value);
    return false;
}

size_t ReadInterval(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return 0;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (value[0] == '-' || errno != 0 || end == value || *end != '\0' ||
        parsed > std::numeric_limits<size_t>::max()) {
        LOG(RTLOG_ERROR, "Unsupported %s=%s; expected a non-negative integer, using 0", name, value);
        return 0;
    }
    return static_cast<size_t>(parsed);
}

bool IsIntervalReached(std::atomic<size_t>& count, size_t interval)
{
    if (interval == 0) {
        return false;
    }
    size_t allocation = count.fetch_add(1, std::memory_order_relaxed) + 1;
    return allocation % interval == 0;
}
} // namespace

void GCDebugConfig::ConfigureFromEnvironment()
{
    clobberEnabled.store(ReadBoolean("MRT_GC_CLOBBER"), std::memory_order_relaxed);
    clobberPositiveControlEnabled.store(
        ReadBoolean("MRT_GC_CLOBBER_POSITIVE_CONTROL"), std::memory_order_relaxed);
    clobberPositiveControlRan.store(false, std::memory_order_relaxed);
    stressMinorInterval.store(ReadInterval("MRT_GC_STRESS_MINOR"), std::memory_order_relaxed);
    stressMajorInterval.store(ReadInterval("MRT_GC_STRESS_MAJOR"), std::memory_order_relaxed);
    stressMinorAllocationCount.store(0, std::memory_order_relaxed);
    stressMajorAllocationCount.store(0, std::memory_order_relaxed);
    stressMinorRequestCount.store(0, std::memory_order_relaxed);
    stressMajorRequestCount.store(0, std::memory_order_relaxed);
    stressMinorExecutionCount.store(0, std::memory_order_relaxed);
    stressMinorYoungExecutionCount.store(0, std::memory_order_relaxed);
    stressMajorExecutionCount.store(0, std::memory_order_relaxed);
}

void GCDebugConfig::DisableStress()
{
    size_t minorRequests = stressMinorRequestCount.load(std::memory_order_relaxed);
    size_t majorRequests = stressMajorRequestCount.load(std::memory_order_relaxed);
    if (minorRequests != 0 || majorRequests != 0) {
        VLOG(REPORT,
            "[GCStress] minorRequests=%zu minorExecutions=%zu minorYoungExecutions=%zu "
            "majorRequests=%zu majorExecutions=%zu",
            minorRequests, stressMinorExecutionCount.load(std::memory_order_relaxed),
            stressMinorYoungExecutionCount.load(std::memory_order_relaxed), majorRequests,
            stressMajorExecutionCount.load(std::memory_order_relaxed));
    }
    stressMinorInterval.store(0, std::memory_order_relaxed);
    stressMajorInterval.store(0, std::memory_order_relaxed);
}

void GCDebugConfig::FillReclaimedMemory(uintptr_t start, size_t size)
{
    if (UNLIKELY(IsClobberEnabled())) {
        MemorySet(start, size, CLOBBER_PATTERN, size);
    }
}

void GCDebugConfig::FillYoungReclaimedMemory(uintptr_t start, size_t size, size_t allocatedSize)
{
    uintptr_t controlTarget = 0;
    bool runPositiveControl = allocatedSize >= sizeof(uintptr_t) && IsClobberPositiveControlEnabled() &&
        !clobberPositiveControlRan.exchange(true, std::memory_order_relaxed);
    if (runPositiveControl) {
        controlTarget = reinterpret_cast<uintptr_t>(reinterpret_cast<BaseObject*>(start)->GetTypeInfo());
    }

    FillReclaimedMemory(start, size);
    if (!runPositiveControl) {
        return;
    }

    uintptr_t observed = *reinterpret_cast<volatile uintptr_t*>(start);
    std::fprintf(stderr,
        "[GCClobberPositiveControl] reclaimed=%p observed=%#zx fill=%#x enabled=%u\n",
        reinterpret_cast<void*>(start), observed, static_cast<unsigned int>(CLOBBER_PATTERN),
        static_cast<unsigned int>(IsClobberEnabled()));
    std::fflush(stderr);
    uintptr_t readTarget = IsClobberEnabled() ? observed : controlTarget;
    volatile uint8_t value = *reinterpret_cast<volatile uint8_t*>(readTarget);
    (void)value;
}

void GCDebugConfig::FillFreePinnedPayload(uintptr_t start, size_t size)
{
    if (UNLIKELY(IsClobberEnabled())) {
        MemorySet(start, size, CLOBBER_PATTERN, size);
    } else {
        MemorySet(start, size, 0, size);
    }
}

bool GCDebugConfig::ShouldTriggerMinor()
{
    return IsIntervalReached(stressMinorAllocationCount, stressMinorInterval.load(std::memory_order_relaxed));
}

bool GCDebugConfig::ShouldTriggerMajor()
{
    return IsIntervalReached(stressMajorAllocationCount, stressMajorInterval.load(std::memory_order_relaxed));
}

void GCDebugConfig::NoteStressMinorRequest()
{
    stressMinorRequestCount.fetch_add(1, std::memory_order_relaxed);
}

void GCDebugConfig::NoteStressMajorRequest()
{
    stressMajorRequestCount.fetch_add(1, std::memory_order_relaxed);
}

void GCDebugConfig::NoteStressMinorExecution(bool wasYoungCollection)
{
    stressMinorExecutionCount.fetch_add(1, std::memory_order_relaxed);
    if (wasYoungCollection) {
        stressMinorYoungExecutionCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void GCDebugConfig::NoteStressMajorExecution()
{
    stressMajorExecutionCount.fetch_add(1, std::memory_order_relaxed);
}
} // namespace MapleRuntime
