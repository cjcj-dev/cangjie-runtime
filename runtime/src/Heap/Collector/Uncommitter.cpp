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
    std::lock_guard<std::mutex> guard(lock);
    const uint64_t now = TimeUtil::NanoSeconds();
    if (canceled.load(std::memory_order_acquire) && now - cancelTime < DelayNs()) {
        return false;
    }
    canceled.store(false, std::memory_order_release);
    cycleStart = now;
    nextUncommitNs = 0;
    uncommitted = 0;
    RegionManager& regions = static_cast<RegionSpace&>(partition).GetRegionManager();
    const size_t committed = regions.GetCommittedCapacity();
    const size_t retain = MinCapacity(regions.GetUsedRegionSize(), kGcTriggerYoungFixedBytes);
    toUncommit = committed > retain ? committed - retain : 0;
    return true;
}

size_t Uncommitter::Uncommit()
{
    if (stopped.load(std::memory_order_acquire) || canceled.load(std::memory_order_acquire) ||
        Heap::GetHeap().IsGcStarted()) {
        return 0;
    }
    RegionManager& regions = static_cast<RegionSpace&>(partition).GetRegionManager();
    const size_t committed = regions.GetCommittedCapacity();
    const size_t retain = MinCapacity(regions.GetUsedRegionSize(), kGcTriggerYoungFixedBytes);
    const size_t release = committed > retain ? committed - retain : 0;
    const size_t flush = std::min({release, toUncommit, ChunkLimit(partition.GetMaxCapacity())});
    // The released cache supplies the age qualification; A02p returns only the
    // actual successful backing delta, including partially completed extents.
    const uint64_t idleBefore = cycleStart > DelayNs() ? cycleStart - DelayNs() : 0;
    return regions.UncommitIdleUnits(flush, idleBefore);
}

void Uncommitter::RegisterUncommit(size_t size)
{
    CHECK(size <= toUncommit);
    toUncommit -= size;
    uncommitted += size;
    nextUncommitNs = 0;
    if (toUncommit == 0 || canceled.load(std::memory_order_acquire)) {
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
        const size_t released = Uncommit();
        if (released == 0) {
            break;
        }
        RegisterUncommit(released);
        if (nextUncommitNs != 0 && !WaitUntil(TimeUtil::NanoSeconds() + nextUncommitNs)) {
            break;
        }
    }
}

void Uncommitter::Run()
{
    uint64_t deadline = TimeUtil::NanoSeconds() + DelayNs();
    while (WaitUntil(deadline)) {
        if (Activate()) {
            RunCycle();
        }
        std::lock_guard<std::mutex> guard(lock);
        deadline = (canceled.load(std::memory_order_acquire) ? cancelTime : cycleStart) + DelayNs();
        toUncommit = 0;
        uncommitted = 0;
        cycleStart = 0;
    }
}

void Uncommitter::CancelCycle()
{
    Uncommitter& self = Current();
    std::lock_guard<std::mutex> guard(self.lock);
    self.cancelTime = TimeUtil::NanoSeconds();
    self.canceled.store(true, std::memory_order_release);
}

bool Uncommitter::CycleCanceled()
{
    return Current().canceled.load(std::memory_order_acquire);
}

bool Uncommitter::ShouldStopUncommit()
{
    return Current().stopped.load(std::memory_order_acquire) || CycleCanceled();
}

bool Uncommitter::ShouldRetryPartial(size_t requestedBytes, size_t releasedBytes)
{
    return releasedBytes < requestedBytes;
}
} // namespace MapleRuntime
