// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Uncommitter.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/GcTrigger.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"

#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/TimeUtils.h"

namespace MapleRuntime {
uint64_t Uncommitter::ParseDelayNs(const char* env)
{
    if (env == nullptr) {
        return kDefaultDelayNs;
    }
    CString raw(env);
    CString s = raw.RemoveBlankSpace();
    if (s.Length() == 0) {
        return kDefaultDelayNs;
    }
    s.ToLowerCase();
    if (s == "0" || s == "0s" || s == "0ms" || s == "0us" || s == "0ns") {
        return 0;
    }
    uint64_t parsed = CString::ParseTimeFromEnv(s);
    if (parsed > 0) {
        return parsed;
    }
    if (CString::IsPosNumber(s)) {
        return std::strtoul(s.Str(), nullptr, 0) * SECOND_TO_NANO_SECOND;
    }
    LOG(RTLOG_ERROR,
        "Unsupported cjUncommitDelay parameter. Use 0 to disable, or a duration with unit (ns/us/ms/s). "
        "Default is 300s.");
    return kDefaultDelayNs;
}

uint64_t Uncommitter::DelayNs()
{
    static const uint64_t delayNs = ParseDelayNs(std::getenv("cjUncommitDelay"));
    return delayNs;
}

size_t Uncommitter::ChunkLimit(size_t maxCapacity)
{
    size_t granule = MRT_PAGE_SIZE == 0 ? 4096 : MRT_PAGE_SIZE;
    size_t byCapacity = (maxCapacity >> 7);
    if (byCapacity < granule) {
        byCapacity = granule;
    } else {
        byCapacity = RoundUp(byCapacity, granule);
    }
    return std::min(byCapacity, kMaxUncommitChunk);
}

size_t Uncommitter::MinCapacity(size_t liveBytes, size_t youngReserve)
{
    size_t sum = liveBytes + youngReserve;
    if (sum < liveBytes) {
        return static_cast<size_t>(-1);
    }
    return sum;
}

Uncommitter& Uncommitter::Current()
{
    // Allocation currently selects logical partition 0. The worker itself is
    // owned by that allocator, and receives its partition at construction.
    return Heap::GetHeap().GetAllocator().GetUncommitter();
}

void Uncommitter::Start()
{
    CHECK(!worker.joinable());
    stopped.store(false, std::memory_order_release);
    worker = std::thread([this] { Run(); });
}

void Uncommitter::Stop()
{
    {
        std::lock_guard<std::mutex> guard(lock);
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }
    if (worker.joinable()) {
        ScopedEnterSaferegion safeRegion(false);
        worker.join();
    }
}

bool Uncommitter::WaitUntil(uint64_t deadline)
{
    std::unique_lock<std::mutex> guard(lock);
    while (!stopped.load(std::memory_order_acquire)) {
        if (!Enabled()) {
            condition.wait(guard);
            continue;
        }
        const uint64_t now = TimeUtil::NanoSeconds();
        if (now >= deadline) {
            return true;
        }
        condition.wait_for(guard, std::chrono::nanoseconds(deadline - now));
    }
    return false;
}

bool Uncommitter::Activate()
{
    RegionManager& regions = static_cast<RegionSpace&>(partition).GetRegionManager();
    ScopedObjectAccess participation;
    std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
    const uint64_t now = TimeUtil::NanoSeconds();
    if (canceled) {
        return false;
    }
    canceled = false;
    cycleStart = now;
    nextUncommitNs = 0;
    uncommitted = 0;
    const size_t committed = regions.GetCommittedCapacity();
    const size_t retain = MinCapacity(regions.pageAllocatorUsed, kGcTriggerYoungFixedBytes);
    toUncommit = committed > retain ? committed - retain : 0;
    return true;
}

size_t Uncommitter::Uncommit()
{
    RegionManager& regions = static_cast<RegionSpace&>(partition).GetRegionManager();
    PageMemory memory;
    {
        // zUncommitter.cpp:367: join before taking the allocation owner.
        // Allocation/cancel and cache claim must not observe separate owners.
        ScopedObjectAccess participation;
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        if (stopped.load(std::memory_order_acquire) || canceled) {
            return 0;
        }
        const size_t committed = regions.GetCommittedCapacity();
        const size_t retain = MinCapacity(regions.pageAllocatorUsed, kGcTriggerYoungFixedBytes);
        const size_t release = committed > retain ? committed - retain : 0;
        const size_t flush = std::min({release, toUncommit, ChunkLimit(partition.GetMaxCapacity())});
        // Cache age selection belongs to A02c; capacity is A02p's backing ledger.
        const uint64_t idleBefore = cycleStart > DelayNs() ? cycleStart - DelayNs() : 0;
        if (!regions.freeRegionManager.TakeUncommitMemory(flush, idleBefore, memory)) {
            Cancel();
            return 0;
        }
    }

    // zUncommitter.cpp:409: system operations run outside the allocator owner
    // and safepoint participation; the claimed extent is not allocatable.
    const size_t completed = RegionInfo::ReleaseUnitsDeferred(memory.index, memory.units);

    {
        // zUncommitter.cpp:415: rejoin, then publish the actual backing prefix
        // and return the extent under the same owner that performed the claim.
        ScopedObjectAccess participation;
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        const size_t released = RegionInfo::PublishUnitsRelease(memory.index, completed);
        regions.freeRegionManager.ReturnUncommitMemory(memory);
        if (released != 0) {
            RegisterUncommit(released);
        } else if (!canceled) {
            Cancel();
        }
        return released;
    }
}

void Uncommitter::RegisterUncommit(size_t size)
{
    CHECK(size <= toUncommit);
    toUncommit -= size;
    uncommitted += size;
    nextUncommitNs = 0;
    if (toUncommit == 0 || canceled) {
        return;
    }
    const uint64_t elapsed = TimeUtil::NanoSeconds() - cycleStart;
    if (elapsed == 0 || elapsed >= DelayNs()) {
        return;
    }
    const double rate = static_cast<double>(uncommitted) / elapsed;
    const double timeToComplete = toUncommit / rate;
    const uint64_t left = DelayNs() - elapsed;
    if (left < timeToComplete) {
        return;
    }
    const size_t remaining = toUncommit / size + 1;
    const uint64_t millisLeft = left / MILLI_SECOND_TO_NANO_SECOND;
    if (remaining < millisLeft) {
        nextUncommitNs = (millisLeft / remaining) * MILLI_SECOND_TO_NANO_SECOND;
    } else {
        const double extra = left - timeToComplete;
        const double random = static_cast<double>(std::rand()) / RAND_MAX;
        nextUncommitNs = random < extra / left ? MILLI_SECOND_TO_NANO_SECOND : 0;
    }
}

void Uncommitter::RunCycle()
{
    while (!stopped.load(std::memory_order_acquire) && toUncommit != 0) {
        if (Uncommit() == 0) {
            break;
        }
        if (toUncommit != 0 && nextUncommitNs != 0 &&
            !WaitUntil(TimeUtil::NanoSeconds() + nextUncommitNs)) {
            break;
        }
    }
}

void Uncommitter::Run()
{
    MutatorManager& mutators = MutatorManager::Instance();
    mutators.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    uint64_t deadline = TimeUtil::NanoSeconds() + DelayNs();
    while (WaitUntil(deadline)) {
        if (Activate()) {
            RunCycle();
        }
        ScopedObjectAccess participation;
        RegionManager& regions = static_cast<RegionSpace&>(partition).GetRegionManager();
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        deadline = (canceled ? cancelTime : cycleStart) + DelayNs();
        toUncommit = 0;
        uncommitted = 0;
        cycleStart = 0;
        canceled = false;
    }
    mutators.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
}

void Uncommitter::Cancel()
{
    // Caller holds this partition's pageAllocatorMutex (ZPartition::reset).
    cancelTime = TimeUtil::NanoSeconds();
    canceled = true;
}

void Uncommitter::CancelCycleLocked()
{
    Current().Cancel();
}
} // namespace MapleRuntime
