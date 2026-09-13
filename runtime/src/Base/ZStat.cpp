// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/ZStat.h"

#include <algorithm>
#include <cmath>
#include "CangjieRuntime.h"
#include "Heap/Heap.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Collector/GcTrigger.h"
#include "Heap/Collector/MutatorAllocRate.h"

namespace MapleRuntime {
void ZStatCycle::Sequence::Add(double value)
{
    if (!initialized) {
        average = value;
        initialized = true;
        return;
    }
    const double difference = value - average;
    average += 0.7 * difference;
    variance = 0.3 * (variance + 0.7 * difference * difference);
}

void ZStatCycle::Initialize(uint64_t now)
{
    std::lock_guard<std::mutex> guard(lock);
    start = end = now;
    initialWorkerDuration = initialWorkerTime = 0;
    warmupCycles = 0;
    lastActiveWorkers = 1;
    serial = Sequence{};
    parallel = Sequence{};
}

void ZStatCycle::AtStart(uint64_t now, uint64_t workerDuration, uint64_t workerTime)
{
    std::lock_guard<std::mutex> guard(lock);
    start = now;
    initialWorkerDuration = workerDuration;
    initialWorkerTime = workerTime;
}

void ZStatCycle::AtEnd(uint64_t now, uint64_t workerDuration, uint64_t workerTime, bool warmup)
{
    std::lock_guard<std::mutex> guard(lock);
    end = now;
    if (warmup && warmupCycles < 3) {
        ++warmupCycles;
    }
    const uint64_t duration = now - start;
    const uint64_t parallelDuration = workerDuration - initialWorkerDuration;
    const uint64_t parallelTime = workerTime - initialWorkerTime;
    serial.Add(static_cast<double>(duration - std::min(duration, parallelDuration)) / SECOND_TO_NANO_SECOND);
    parallel.Add(static_cast<double>(parallelTime) / SECOND_TO_NANO_SECOND);
    lastActiveWorkers = parallelDuration == 0 ? 1.0 :
        static_cast<double>(parallelTime) / parallelDuration;
}

ZStatCycleStats ZStatCycle::Stats(uint64_t now) const
{
    std::lock_guard<std::mutex> guard(lock);
    return {warmupCycles, static_cast<double>(now - std::min(now, end)) / SECOND_TO_NANO_SECOND,
            serial.average, std::sqrt(serial.variance), parallel.average, std::sqrt(parallel.variance),
            lastActiveWorkers};
}

void ZStatCollection::AtYoungMarkStart(bool startsOld)
{
    std::lock_guard<std::mutex> guard(lock);
    ++counts.totalCollections;
    if (startsOld) {
        counts.collectionsAtMajorStart = counts.totalCollections;
    }
}

ZStatCollectionStats ZStatCollection::Stats() const
{
    std::lock_guard<std::mutex> guard(lock);
    return counts;
}

GcTriggerInputs ZStat::SampleDirectorStats(uint64_t now, ZStatCycle& young, ZStatCycle& old,
                                    RegionManager& regions, GCWorkers& youngWorkers, GCWorkers& oldWorkers,
                                    const ZStatCollection& collections)
{
    const auto youngState = youngWorkers.GetSnapshot();
    const auto oldState = oldWorkers.GetSnapshot();
    const uint32_t concurrentWorkers = youngState.capacity;
    const auto rate = MutatorAllocRate::stats();
    const auto youngCycle = young.Stats(now);
    const auto oldCycle = old.Stats(now);
    GcTriggerInputs in;
    in.workerCapacity = concurrentWorkers;
    in.youngWorkersActive = youngState.cycleActive;
    in.oldWorkersActive = oldState.cycleActive;
    in.activeYoungWorkers = youngState.cycleActive ? youngState.activeWorkers : 0;
    in.activeOldWorkers = oldState.cycleActive ? oldState.activeWorkers : 0;
    in.allocationStalling = regions.IsAllocationStalling();
    in.allocRateAvgBps = rate.avg;
    in.allocRatePredictBps = rate.predict;
    in.allocRateSdBps = rate.sd;
    in.usedBytes = Heap::GetHeap().GetAllocator().AllocatedBytes();
    in.youngUsedBytes = regions.GetYoungAllocatedSize();
    in.oldUsedBytes = in.usedBytes - std::min(in.usedBytes, in.youngUsedBytes);
    in.capacityBytes = Heap::GetHeap().GetMaxCapacity();
    in.softMaxBytes = MutatorAllocRate::soft_max_heap_size();
    // zHeuristics.cpp:61-66: one small relocation page per concurrent
    // worker. This allocator has no shared medium-page/NUMA allocation tier.
    in.relocationHeadroomBytes = concurrentWorkers * regions.GetThreadLocalRegionSize();
    in.youngSerialTimeSec = youngCycle.serialTime + youngCycle.serialTimeSd * kGcTriggerOneIn1000;
    in.youngParallelTimeSec = youngCycle.parallelTime + youngCycle.parallelTimeSd * kGcTriggerOneIn1000;
    in.lastYoungGcDurationSec = in.youngSerialTimeSec + in.youngParallelTimeSec;
    in.lastYoungWorkers = youngCycle.lastActiveWorkers;
    in.lastOldGcDurationSec = oldCycle.serialTime + oldCycle.serialTimeSd * kGcTriggerOneIn1000 +
        oldCycle.parallelTime + oldCycle.parallelTimeSd * kGcTriggerOneIn1000;
    in.lastGcDurationSec = in.youngSerialTimeSec + in.youngParallelTimeSec / concurrentWorkers;
    in.timeSinceLastGcSec = youngCycle.timeSinceLast;
    in.timeSinceLastMajorSec = oldCycle.timeSinceLast;
    in.collectionIntervalSec = static_cast<double>(CangjieRuntime::GetGCParam().backupGCInterval) /
        SECOND_TO_NANO_SECOND;
    in.warmupCyclesDone = oldCycle.warmupCycles;
    in.isWarm = oldCycle.warmupCycles >= 3;
    in.isTimeTrustable = oldCycle.warmupCycles > 0;
    const auto collectionStats = collections.Stats();
    in.totalCollections = collectionStats.totalCollections;
    in.collectionsAtLastMajor = collectionStats.collectionsAtMajorStart;
    in.usedAtLastMajorEnd = GCStats::usedAtLastMajorEnd.load(std::memory_order_relaxed);
    in.oldLiveAtMarkEnd = GCStats::oldLiveAtMarkEnd.load(std::memory_order_relaxed);
    in.reclaimedPerYoungAvg = GCStats::reclaimedPerYoungAvg.load(std::memory_order_relaxed);
    in.reclaimedPerOldAvg = GCStats::reclaimedPerOldAvg.load(std::memory_order_relaxed);
    return in;
}
} // namespace MapleRuntime

#if MRT_ZSTAT_COMPILED

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {
std::atomic<int> ZStat::g_stwDepth{ 0 };
std::atomic<int> ZStat::g_enabledOverride{ -1 };

std::mutex& ZStat::TableLock()
{
    static std::mutex lock;
    return lock;
}

ZStat::Table& ZStat::CycleTable()
{
    static Table table;
    return table;
}

bool ZStat::Enabled()
{
    int overrideValue = g_enabledOverride.load(std::memory_order_acquire);
    if (overrideValue >= 0) {
        return overrideValue == 1;
    }
    static const bool enabled = []() {
        const char* env = std::getenv("MRT_ZSTAT");
        return env != nullptr && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

void ZStat::EnterStwScope()
{
    if (Enabled()) {
        g_stwDepth.fetch_add(1, std::memory_order_relaxed);
    }
}

void ZStat::ExitStwScope()
{
    if (Enabled()) {
        g_stwDepth.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool ZStat::WorldStoppedNow()
{
    return g_stwDepth.load(std::memory_order_acquire) > 0;
}

void ZStat::NotePhase(const char* name, bool worldStoppedAtStart, uint64_t ns)
{
    if (name == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(TableLock());
    Table& table = CycleTable();
    PhaseTotals& totals = table.phases[name];
    if (worldStoppedAtStart) {
        totals.pauseNs += ns;
        ++totals.nPause;
        if (ns > totals.maxPauseNs) {
            totals.maxPauseNs = ns;
        }
        table.pauseNs += ns;
        if (ns > table.maxPauseNs) {
            table.maxPauseNs = ns;
        }
    } else {
        totals.concNs += ns;
        ++totals.nConc;
        table.concNs += ns;
    }
}

void ZStat::NoteCycleEnd(uint64_t seq)
{
    if (!Enabled()) {
        return;
    }
    Table snapshot;
    {
        std::lock_guard<std::mutex> guard(TableLock());
        snapshot.phases = CycleTable().phases;
        snapshot.pauseNs = CycleTable().pauseNs;
        snapshot.concNs = CycleTable().concNs;
        snapshot.maxPauseNs = CycleTable().maxPauseNs;
        CycleTable() = Table();
    }
    // Deterministic emission order: phase names sorted, so two runs of the same workload diff
    // line-by-line without a sort step.
    std::vector<std::string> names;
    names.reserve(snapshot.phases.size());
    for (const auto& kv : snapshot.phases) {
        names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const PhaseTotals& totals = snapshot.phases.at(name);
        char safe[128]; // 128: same cap as GcLog::MAX_PHASE_NAME
        FoldToToken(name.c_str(), safe, sizeof(safe));
        EmitLine("[ZSTAT] v=1 rec=zphase seq=%llu name=%s pause_ns=%llu conc_ns=%llu n=%u",
                 static_cast<unsigned long long>(seq), safe,
                 static_cast<unsigned long long>(totals.pauseNs),
                 static_cast<unsigned long long>(totals.concNs), totals.nPause + totals.nConc);
    }
    EmitLine("[ZSTAT] v=1 rec=zcycle seq=%llu pause_ns=%llu conc_ns=%llu max_pause_ns=%llu phases=%zu",
             static_cast<unsigned long long>(seq), static_cast<unsigned long long>(snapshot.pauseNs),
             static_cast<unsigned long long>(snapshot.concNs),
             static_cast<unsigned long long>(snapshot.maxPauseNs), snapshot.phases.size());
}

ZStat::PhaseTotals ZStat::Phase(const char* name)
{
    std::lock_guard<std::mutex> guard(TableLock());
    auto it = CycleTable().phases.find(name);
    if (it == CycleTable().phases.end()) {
        return PhaseTotals{};
    }
    return it->second;
}

std::vector<std::string> ZStat::RegisteredPhases()
{
    std::lock_guard<std::mutex> guard(TableLock());
    std::vector<std::string> names;
    names.reserve(CycleTable().phases.size());
    for (const auto& kv : CycleTable().phases) {
        names.push_back(kv.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

uint64_t ZStat::CyclePauseNs()
{
    std::lock_guard<std::mutex> guard(TableLock());
    return CycleTable().pauseNs;
}

uint64_t ZStat::CycleConcNs()
{
    std::lock_guard<std::mutex> guard(TableLock());
    return CycleTable().concNs;
}

uint64_t ZStat::CycleMaxPauseNs()
{
    std::lock_guard<std::mutex> guard(TableLock());
    return CycleTable().maxPauseNs;
}

void ZStat::SetEnabledForTest(bool enabled)
{
    g_enabledOverride.store(enabled ? 1 : 0, std::memory_order_release);
}

void ZStat::ResetForTest()
{
    std::lock_guard<std::mutex> guard(TableLock());
    CycleTable() = Table();
}

void ZStat::FoldToToken(const char* text, char* out, size_t cap)
{
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < cap && text[i] != '\0'; ++i) {
            char c = text[i];
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
            out[i] = keep ? c : '_';
        }
    }
    out[i] = '\0';
}

void ZStat::EmitLine(const char* format, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    if (static_cast<size_t>(n) >= sizeof(buf)) {
        n = static_cast<int>(sizeof(buf) - 1);
    }
    buf[n] = '\0';
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
}
} // namespace MapleRuntime
#endif // MRT_ZSTAT_COMPILED
