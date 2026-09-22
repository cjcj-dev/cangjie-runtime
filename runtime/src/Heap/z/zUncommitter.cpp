// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zUncommitter.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>

#include "Heap/Allocator/RegionSpace.h"
#include "Base/Globals.h"
#include "Common/ScopedObjectAccess.h"
#include "Mutator/MutatorManager.h"

#include "Base/CString.h"
#include "Base/Log.h"
#include <cstring>
#include "Base/TimeUtils.h"

namespace MapleRuntime {
// The runtime is compiled as gnu++14 (runtime/config.cmake:368). There an
// in-class static constexpr declaration is not a definition, and std::min(const
// T&, const T&) in ChunkLimit odr-uses kMaxUncommitChunk, so the Debug (-O0)
// link reports an undefined reference; -O2 only hides it by constant folding.
// The reference JDK builds HotSpot as -std=c++17 (make/autoconf/flags-cflags.m4:615),
// where such members are implicitly inline and zMappedCache.hpp:87-90 needs no
// out-of-class line. Same idiom as zVirtualMemoryManager.cpp:40 here and
// HotSpot memory/metaspace/blockTree.cpp:37. kDefaultDelayNs is only read by
// value in this TU (no odr-use measured), so it gets no definition.
constexpr size_t Uncommitter::kMaxUncommitChunk;

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

static std::atomic<uint64_t>& UncommitDelayNsStorage()
{
    static std::atomic<uint64_t> delayNs{ Uncommitter::ParseDelayNs(std::getenv("cjUncommitDelay")) };
    return delayNs;
}

static std::atomic<bool>& UncommitFlagStorage()
{
    static std::atomic<bool> enabled{ [] {
        const char* env = std::getenv("cjUncommit");
        return env == nullptr || std::strcmp(env, "0") != 0;
    }() };
    return enabled;
}

uint64_t Uncommitter::DelayNs()
{
    return UncommitDelayNsStorage().load(std::memory_order_relaxed);
}

bool Uncommitter::ZUncommit()
{
    return UncommitFlagStorage().load(std::memory_order_relaxed);
}

void Uncommitter::SetZUncommit(bool enabled)
{
    UncommitFlagStorage().store(enabled, std::memory_order_relaxed);
}

size_t Uncommitter::ZUncommitDelay()
{
    return static_cast<size_t>(DelayNs() / SECOND_TO_NANO_SECOND);
}

void Uncommitter::SetZUncommitDelay(size_t seconds)
{
    UncommitDelayNsStorage().store(static_cast<uint64_t>(seconds) * SECOND_TO_NANO_SECOND, std::memory_order_relaxed);
}

size_t Uncommitter::ChunkLimit(size_t maxCapacity)
{
    // zUncommitter.cpp:379-380: chunk limits are whole heap granules.
    const size_t upper = std::max(ZGranuleSize,
        AlignDown(kMaxUncommitChunk / ZPerNUMAStorage::count(), ZGranuleSize));
    return std::min(AlignUp(maxCapacity >> 7, ZGranuleSize), upper);
}

size_t Uncommitter::MinCapacity(size_t liveBytes, size_t youngReserve)
{
    size_t sum = liveBytes + youngReserve;
    if (sum < liveBytes) {
        return static_cast<size_t>(-1);
    }
    return sum;
}

// zUncommitter.cpp:41-56: set_name + create_and_start.
void Uncommitter::Start()
{
    CHECK(!started);
    stopped.store(false, std::memory_order_release);
    started = true;
    char name[64];
    std::snprintf(name, sizeof(name), "ZUncommitter#%u", partition.numaId);
    set_name(name);
    create_and_start();
}

// ConcurrentGCThread::stop; the wait for termination runs in a safe region
// because the uncommitter thread participates in safepoints (I17). A set
// that was never started has no thread to stop; only its wait is released.
void Uncommitter::Stop()
{
    if (!started) {
        terminate();
        return;
    }
    started = false;
    ScopedEnterSaferegion safeRegion(false);
    stop();
}

// zUncommitter.cpp:171-175
void Uncommitter::terminate()
{
    std::lock_guard<std::mutex> guard(lock);
    stopped.store(true, std::memory_order_release);
    condition.notify_all();
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
    RegionManager& regions = partition.regionManager;
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
    const size_t retain = MinCapacity(regions.pageAllocatorUsed, 32 * MB);
    toUncommit = committed > retain ? committed - retain : 0;
    return true;
}

size_t Uncommitter::Uncommit()
{
    RegionManager& regions = partition.regionManager;
    ZArray<ZVirtualMemory> flushedVmems;
    size_t flushed = 0;
    {
        // zUncommitter.cpp:367: join before taking the allocation owner.
        // Allocation/cancel and cache claim must not observe separate owners.
        ScopedObjectAccess participation;
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        if (stopped.load(std::memory_order_acquire) || canceled) {
            return 0;
        }
        const size_t committed = regions.GetCommittedCapacity();
        const size_t retain = MinCapacity(regions.pageAllocatorUsed, 32 * MB);
        const size_t release = committed > retain ? committed - retain : 0;
        const size_t flush = std::min({release, toUncommit, ChunkLimit(Heap::GetHeap().GetMaxCapacity())});
        // zUncommitter.cpp:395: flush memory from the mapped cache for uncommit.
        flushed = regions.freeRegionManager.RemoveForUncommit(flush, &flushedVmems);
        if (flushed == 0) {
            Cancel();
            return 0;
        }
    }

    // zUncommitter.cpp:405-411: unmap and uncommit flushed memory outside the
    // allocator owner and safepoint participation; the claimed extents are not
    // allocatable.
    for (const ZVirtualMemory vmem : flushedVmems) {
        const uint32_t partitionId = regions.freeRegionManager.PartitionIdOf(vmem);
        regions.freeRegionManager.unmap_virtual(vmem);
        regions.freeRegionManager.uncommit_physical(vmem);
        regions.freeRegionManager.free_physical(vmem, partitionId);
        regions.freeRegionManager.free_virtual(vmem, partitionId);
    }

    {
        // zUncommitter.cpp:413-420: rejoin, adjust claimed and capacity.
        ScopedObjectAccess participation;
        std::lock_guard<std::mutex> guard(regions.pageAllocatorMutex);
        regions.freeRegionManager.UncommitFlushed(flushed);
        RegisterUncommit(flushed);
        return flushed;
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

// zUncommitter.cpp:109-169
void Uncommitter::run_thread()
{
    MutatorManager& mutators = MutatorManager::Instance();
    mutators.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    uint64_t deadline = TimeUtil::NanoSeconds() + DelayNs();
    while (WaitUntil(deadline)) {
        if (Activate()) {
            RunCycle();
        }
        ScopedObjectAccess participation;
        RegionManager& regions = partition.regionManager;
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

} // namespace MapleRuntime

namespace MapleRuntime {
Uncommitter::Uncommitter(ZPartition& partition) : partition(partition) {}
}
