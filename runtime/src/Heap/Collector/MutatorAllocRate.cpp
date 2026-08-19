// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MutatorAllocRate.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Base/AtomicSpinLock.h"
#include "Base/Globals.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/GcTrigger.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
std::atomic<uint64_t> g_gcTriggerArmed{ 0 };
std::atomic<uint64_t> g_gcTriggerTurned{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleTimer{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleWarmup{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleAllocRate{ 0 };
std::atomic<uint64_t> g_gcTriggerRuleHighUsage{ 0 };

namespace {
AtomicSpinLock g_statLock;
uint64_t g_lastSampleTimeNs = 0;
std::atomic<size_t> g_samplingGranule{ 1 };
std::atomic<size_t> g_allocatedSinceSample{ 0 };
std::atomic<uint64_t> g_sampleCount{ 0 };
TruncatedSeq g_samplesTime(100);
TruncatedSeq g_samplesBytes(100);
TruncatedSeq g_rate(100);
} // namespace

void MutatorAllocRate::update_sampling_granule()
{
    // zStat.cpp:951-955 — sampling_heap_granules = 128, align_up to granule size.
    constexpr size_t samplingHeapGranules = 128;
    size_t softMax = Heap::GetHeap().GetMaxCapacity();
    if (softMax == 0) {
        softMax = 256 * MB;
    }
    size_t granule = softMax / samplingHeapGranules;
    const size_t unit = RegionInfo::UNIT_SIZE == 0 ? 4096 : RegionInfo::UNIT_SIZE;
    granule = AlignUp(granule, unit);
    if (granule == 0) {
        granule = unit;
    }
    g_samplingGranule.store(granule, std::memory_order_release);
}

void MutatorAllocRate::initialize()
{
    g_lastSampleTimeNs = TimeUtil::NanoSeconds();
    g_allocatedSinceSample.store(0, std::memory_order_relaxed);
    g_sampleCount.store(0, std::memory_order_relaxed);
    g_samplesTime.reset();
    g_samplesBytes.reset();
    g_rate.reset();
    update_sampling_granule();
    static std::atomic<bool> dumped{ false };
    bool expected = false;
    if (dumped.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            VLOG(REPORT,
                 "[GCV2][gctrigger] atexit armed=%llu turned=%llu timer=%llu warmup=%llu alloc_rate=%llu "
                 "high_usage=%llu samples=%llu granule=%zu",
                 static_cast<unsigned long long>(g_gcTriggerArmed.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_gcTriggerTurned.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_gcTriggerRuleTimer.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_gcTriggerRuleWarmup.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_gcTriggerRuleAllocRate.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_gcTriggerRuleHighUsage.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(MutatorAllocRate::sample_count()),
                 MutatorAllocRate::sampling_granule());
        });
    }
}

void MutatorAllocRate::sample_allocation(size_t allocationBytes)
{
    // zStat.cpp:957-1012
    const size_t allocated = g_allocatedSinceSample.fetch_add(allocationBytes, std::memory_order_relaxed) +
        allocationBytes;
    if (allocated < g_samplingGranule.load(std::memory_order_relaxed)) {
        return;
    }
    if (!g_statLock.TryLock()) {
        return;
    }
    const size_t allocatedSample = g_allocatedSinceSample.load(std::memory_order_relaxed);
    if (allocatedSample < g_samplingGranule.load(std::memory_order_relaxed)) {
        g_statLock.Unlock();
        return;
    }
    const uint64_t now = TimeUtil::NanoSeconds();
    const uint64_t elapsed = now - g_lastSampleTimeNs;
    if (elapsed == 0) {
        g_statLock.Unlock();
        return;
    }
    g_allocatedSinceSample.fetch_sub(allocatedSample, std::memory_order_relaxed);
    g_samplesTime.add(static_cast<double>(elapsed));
    g_samplesBytes.add(static_cast<double>(allocatedSample));
    const double lastSampleBytes = g_samplesBytes.sum();
    const double elapsedNs = g_samplesTime.sum();
    const double elapsedSeconds = elapsedNs / static_cast<double>(SECOND_TO_NANO_SECOND);
    const double bytesPerSecond = elapsedSeconds <= 0.0 ? 0.0 : lastSampleBytes / elapsedSeconds;
    g_rate.add(bytesPerSecond);
    update_sampling_granule();
    g_lastSampleTimeNs = now;
    g_sampleCount.fetch_add(1, std::memory_order_relaxed);
    g_statLock.Unlock();
}

MutatorAllocRateStats MutatorAllocRate::stats()
{
    g_statLock.Lock();
    MutatorAllocRateStats out;
    out.avg = g_rate.avg();
    out.predict = g_rate.predict_next();
    out.sd = g_rate.sd();
    g_statLock.Unlock();
    return out;
}

size_t MutatorAllocRate::sampling_granule() { return g_samplingGranule.load(std::memory_order_acquire); }

uint64_t MutatorAllocRate::sample_count() { return g_sampleCount.load(std::memory_order_relaxed); }
} // namespace MapleRuntime
