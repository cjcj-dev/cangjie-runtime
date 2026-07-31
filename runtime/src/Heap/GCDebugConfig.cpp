// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GCDebugConfig.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Allocator/MemMap.h"
#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/StateWord.h"

namespace MapleRuntime {
std::atomic<bool> GCDebugConfig::clobberEnabled{ false };
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
MemMap* clobberGuard = nullptr;
uintptr_t clobberAddress = 0;

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

void InitializeClobberGuard()
{
    MemMap::Option option = MemMap::DEFAULT_OPTIONS;
    option.tag = "cangjie_gc_clobber_guard";
    option.prot = MemMap::PROT_NONE;
    option.protAll = true;
    clobberGuard = MemMap::MapMemory(MRT_PAGE_SIZE, MRT_PAGE_SIZE, option);
    clobberAddress = reinterpret_cast<uintptr_t>(clobberGuard->GetBaseAddr());
    CHECK_DETAIL(clobberAddress != 0, "clobber guard must not use the null address");
#if UINTPTR_MAX > UINT32_MAX
    constexpr uintptr_t addressMask = (static_cast<uintptr_t>(1) << StateWord::ADDRESS_BIT_COUNT) - 1;
    CHECK_DETAIL((clobberAddress & ~addressMask) == 0,
        "clobber guard address %#zx does not survive the 48-bit managed-reference encoding", clobberAddress);
#endif
    VLOG(REPORT, "[GCClobber] guard=[%#zx,%#zx) encoded=%#zx",
        clobberAddress, clobberAddress + MRT_PAGE_SIZE, clobberAddress);
}
} // namespace

void GCDebugConfig::ConfigureFromEnvironment()
{
    clobberEnabled.store(ReadBoolean("MRT_GC_CLOBBER"), std::memory_order_relaxed);
    if (IsClobberEnabled()) {
        InitializeClobberGuard();
    }
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

void GCDebugConfig::Finalize()
{
    MemMap::DestroyMemMap(clobberGuard);
    clobberAddress = 0;
    clobberEnabled.store(false, std::memory_order_relaxed);
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

bool GCDebugConfig::FillReclaimedMemory(uintptr_t start, size_t size)
{
    if (!IsClobberEnabled()) {
        return false;
    }
    CHECK_DETAIL(clobberAddress != 0, "clobber guard is not initialized");
    CHECK_DETAIL(start % alignof(uintptr_t) == 0 && size % sizeof(uintptr_t) == 0,
        "clobber range [%#zx+%zu) must be word aligned", start, size);
    uintptr_t* cursor = reinterpret_cast<uintptr_t*>(start);
    uintptr_t* end = cursor + size / sizeof(uintptr_t);
    while (cursor != end) {
        *cursor++ = clobberAddress;
    }
    return true;
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
