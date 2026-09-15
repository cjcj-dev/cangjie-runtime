// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zStat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <thread>
#include "CangjieRuntime.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zDirector.hpp"

namespace MapleRuntime {
// zStat.cpp:65-240: rolling ten-second, ten-minute and ten-hour windows.
struct ZStatSamplerData {
    uint64_t nsamples = 0;
    uint64_t sum = 0;
    uint64_t max = 0;
    void Add(const ZStatSamplerData& value);
    uint64_t Average() const { return nsamples == 0 ? 0 : sum / nsamples; }
};

template<size_t Size>
class ZStatSamplerHistoryInterval {
public:
    bool Add(const ZStatSamplerData& sample)
    {
        const auto old = samples[next];
        samples[next] = sample;
        accumulated.Add(sample);
        total.nsamples += sample.nsamples - old.nsamples;
        total.sum += sample.sum - old.sum;
        if (total.max < sample.max) {
            total.max = sample.max;
        } else if (total.max == old.max) {
            total.max = 0;
            for (const auto& value : samples) total.max = std::max(total.max, value.max);
        }
        if (++next == Size) {
            next = 0;
            accumulated = {};
            return true;
        }
        return false;
    }
    const ZStatSamplerData& Total() const { return total; }
    const ZStatSamplerData& Accumulated() const { return accumulated; }
private:
    size_t next = 0;
    std::array<ZStatSamplerData, Size> samples {};
    ZStatSamplerData accumulated;
    ZStatSamplerData total;
};

class ZStatSamplerHistory {
public:
    void Add(const ZStatSamplerData& sample);
    std::array<ZStatSamplerData, 4> Windows() const
    {
        auto minute = minutes.Total();
        minute.Add(seconds.Accumulated());
        auto hour = hours.Total();
        hour.Add(minutes.Accumulated());
        hour.Add(seconds.Accumulated());
        auto all = total;
        all.Add(hours.Accumulated());
        all.Add(minutes.Accumulated());
        all.Add(seconds.Accumulated());
        return {seconds.Total(), minute, hour, all};
    }
private:
    ZStatSamplerHistoryInterval<10> seconds;
    ZStatSamplerHistoryInterval<60> minutes;
    ZStatSamplerHistoryInterval<60> hours;
    ZStatSamplerData total;
};


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

// zStat.cpp:1330-1405
ZStatWorkers::ZStatWorkers()
    : _stat_lock(), _active_workers(0), _start_of_last(0), _accumulated_duration(0), _accumulated_time(0) {}

void ZStatWorkers::at_start(uint32_t active_workers)
{
    std::lock_guard<std::mutex> locker(_stat_lock);
    _start_of_last = TimeUtil::NanoSeconds();
    _active_workers = active_workers;
}

void ZStatWorkers::at_end()
{
    std::lock_guard<std::mutex> locker(_stat_lock);
    const uint64_t now = TimeUtil::NanoSeconds();
    const uint64_t duration = now - _start_of_last;
    uint64_t time = duration;
    for (uint32_t i = 1; i < _active_workers; ++i) {
        time += duration;
    }
    _accumulated_time += time;
    _accumulated_duration += duration;
    _active_workers = 0;
}

double ZStatWorkers::accumulated_time()
{
    const uint32_t nworkers = _active_workers;
    const uint64_t now = TimeUtil::NanoSeconds();
    const uint64_t start = _start_of_last;
    uint64_t time = _accumulated_time;
    if (nworkers != 0) {
        for (uint32_t i = 0; i < nworkers; ++i) {
            time += now - start;
        }
    }
    return static_cast<double>(time) / SECOND_TO_NANO_SECOND;
}

double ZStatWorkers::accumulated_duration()
{
    const uint64_t now = TimeUtil::NanoSeconds();
    const uint64_t start = _start_of_last;
    uint64_t duration = _accumulated_duration;
    if (_active_workers != 0) {
        duration += now - start;
    }
    return static_cast<double>(duration) / SECOND_TO_NANO_SECOND;
}

uint32_t ZStatWorkers::active_workers()
{
    return _active_workers;
}

double ZStatWorkers::get_and_reset_duration()
{
    std::lock_guard<std::mutex> locker(_stat_lock);
    const double duration = static_cast<double>(_accumulated_duration) / SECOND_TO_NANO_SECOND;
    _accumulated_duration = 0;
    return duration;
}

double ZStatWorkers::get_and_reset_time()
{
    std::lock_guard<std::mutex> locker(_stat_lock);
    const double time = static_cast<double>(_accumulated_time) / SECOND_TO_NANO_SECOND;
    _accumulated_time = 0;
    return time;
}

ZStatWorkersStats ZStatWorkers::stats()
{
    std::lock_guard<std::mutex> locker(_stat_lock);
    return { accumulated_time(), accumulated_duration() };
}

void ZStatCycle::Initialize(uint64_t now)
{
    std::lock_guard<std::mutex> guard(lock);
    start = end = now;
    warmupCycles = 0;
    lastActiveWorkers = 1;
    serial = Sequence{};
    parallel = Sequence{};
}

// zStat.cpp:1237-1240
void ZStatCycle::AtStart(uint64_t now)
{
    std::lock_guard<std::mutex> guard(lock);
    start = now;
}

// zStat.cpp:1242-1268
void ZStatCycle::AtEnd(uint64_t now, ZStatWorkers* statWorkers, bool warmup, bool recordStats)
{
    std::lock_guard<std::mutex> guard(lock);
    end = now;
    if (warmup && warmupCycles < 3) {
        ++warmupCycles;
    }
    // Calculate serial and parallelizable GC cycle times
    const double duration = static_cast<double>(now - start) / SECOND_TO_NANO_SECOND;
    const double workersDuration = statWorkers->get_and_reset_duration();
    const double workersTime = statWorkers->get_and_reset_time();
    const double serialTime = duration - std::min(duration, workersDuration);
    lastActiveWorkers = workersDuration > 0.0 ? workersTime / workersDuration : 1.0;
    if (recordStats) {
        serial.Add(serialTime);
        parallel.Add(workersTime);
    }
}

ZStatCycleStats ZStatCycle::Stats(uint64_t now) const
{
    std::lock_guard<std::mutex> guard(lock);
    return {warmupCycles, static_cast<double>(now - std::min(now, end)) / SECOND_TO_NANO_SECOND,
            serial.average, std::sqrt(serial.variance), parallel.average, std::sqrt(parallel.variance),
            lastActiveWorkers};
}

ZStatCollection& ZStat::Collections()
{
    static ZStatCollection collections;
    return collections;
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

namespace {
// zDirector.cpp:651-678: read is_active and active_workers under the
// resizing lock; an inactive generation contributes no worker count.
struct WorkerResizeSample {
    bool isActive;
    uint32_t activeWorkers;
};

WorkerResizeSample SampleWorkerResizeStats(ZWorkers& workers)
{
    std::lock_guard<std::mutex> locker(*workers.resizing_lock());
    if (!workers.is_active()) {
        // If the workers are not active, it isn't safe to read stats
        // from the stat_cycle, so return early.
        return { false, 0 };
    }
    return { true, workers.active_workers() };
}
} // namespace

GcTriggerInputs ZStat::SampleDirectorStats(uint64_t now, ZStatCycle& young, ZStatCycle& old,
                                    RegionManager& regions, ZWorkers& youngWorkers, ZWorkers& oldWorkers,
                                    uint32_t workerCapacity)
{
    const WorkerResizeSample youngState = SampleWorkerResizeStats(youngWorkers);
    const WorkerResizeSample oldState = SampleWorkerResizeStats(oldWorkers);
    const uint32_t concurrentWorkers = workerCapacity;
    const auto rate = ZStatMutatorAllocRate::stats();
    const auto youngCycle = young.Stats(now);
    const auto oldCycle = old.Stats(now);
    GcTriggerInputs in;
    in.workerCapacity = concurrentWorkers;
    in.youngWorkersActive = youngState.isActive;
    in.oldWorkersActive = oldState.isActive;
    in.activeYoungWorkers = youngState.activeWorkers;
    in.activeOldWorkers = oldState.activeWorkers;
    in.allocationStalling = regions.IsAllocationStalling();
    in.allocRateAvgBps = rate.avg;
    in.allocRatePredictBps = rate.predict;
    in.allocRateSdBps = rate.sd;
    in.usedBytes = Heap::GetHeap().GetAllocator().AllocatedBytes();
    in.youngUsedBytes = regions.GetYoungAllocatedSize();
    in.oldUsedBytes = in.usedBytes - std::min(in.usedBytes, in.youngUsedBytes);
    in.capacityBytes = Heap::GetHeap().GetMaxCapacity();
    in.softMaxBytes = ZStatMutatorAllocRate::soft_max_heap_size();
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
    const auto collectionStats = Collections().Stats();
    in.totalCollections = collectionStats.totalCollections;
    in.collectionsAtLastMajor = collectionStats.collectionsAtMajorStart;
    const auto youngHeap = YoungHeap().Stats();
    const auto oldHeap = OldHeap().Stats();
    in.usedAtLastMajorEnd = oldHeap.usedAtRelocateEnd;
    in.oldLiveAtMarkEnd = oldHeap.liveAtMarkEnd;
    in.reclaimedPerYoungAvg = youngHeap.reclaimedAverage;
    in.reclaimedPerOldAvg = oldHeap.reclaimedAverage;
    return in;
}
} // namespace MapleRuntime


#include <chrono>
#include <cstdio>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#include <unistd.h>
#endif
#include "Base/LogFile.h"

namespace MapleRuntime {
ZStatSampler* ZStatSampler::first = nullptr;
uint32_t ZStatSampler::count = 0;
ZStatCounter* ZStatCounter::first = nullptr;
uint32_t ZStatCounter::count = 0;
std::atomic<int> ZStat::stwDepth {0};
size_t ZStatValue::stride = 0;
char* ZStatValue::base = nullptr;

ZStatValue::ZStatValue(const char* group, const char* name, uint32_t id, size_t size)
    : group(group), name(name), id(id), offset(stride)
{
    MRT_ASSERT(base == nullptr, "statistics registered after initialization");
    stride += size;
}

void ZStatValue::InitializeStorage()
{
    // Each CpuData is cache-line aligned; the entire per-CPU layout is resident.
    stride = (stride + 63) & ~static_cast<size_t>(63);
    // zUtils.inline.hpp:37-49: reserve padding and keep the aligned storage
    // for process lifetime. This also works in the product's C++14 build.
    const uintptr_t allocation = reinterpret_cast<uintptr_t>(std::malloc(stride * CpuCount() + 63));
    CHECK_DETAIL(allocation != 0, "statistics storage allocation failed");
    base = reinterpret_cast<char*>((allocation + 63) & ~static_cast<uintptr_t>(63));
}

size_t ZStatValue::CpuCount()
{
    static const size_t count = [] {
#if defined(__linux__) || defined(hongmeng)
        const long configured = sysconf(_SC_NPROCESSORS_CONF);
        if (configured > 0) return static_cast<size_t>(configured);
#endif
        return static_cast<size_t>(std::max(1U, std::thread::hardware_concurrency()));
    }();
    return count;
}

size_t ZStatValue::CpuId()
{
#if defined(__linux__) || defined(hongmeng)
    const int cpu = sched_getcpu();
    if (cpu >= 0 && static_cast<size_t>(cpu) < CpuCount()) return static_cast<size_t>(cpu);
#endif
    // os_bsd.cpp:2260-2265 / os_linux.cpp:4987-5008: unsupported or invalid
    // processor ids share CPU zero; all updates remain atomic.
    return 0;
}

ZStatSampler::ZStatSampler(const char* group, const char* name, ZStatUnit unit)
    : ZStatValue(group, name, count++, sizeof(CpuData)), next(first), unit(unit)
{
    first = this;
}

// zStat.cpp:421-447: sort the resident registry once, preserving metric ids.
void ZStatSampler::Sort()
{
    auto* unsorted = first;
    first = nullptr;
    while (unsorted != nullptr) {
        auto* value = unsorted;
        unsorted = value->next;
        auto** current = &first;
        while (*current != nullptr) {
            const int group = std::strcmp((*current)->Group(), value->Group());
            if (group > 0 || (group == 0 && std::strcmp((*current)->Name(), value->Name()) > 0)) break;
            current = &(*current)->next;
        }
        value->next = *current;
        *current = value;
    }
}

void ZStatSampler::Initialize() const
{
    for (size_t i = 0; i < CpuCount(); ++i) new (CpuLocal<CpuData>(i)) CpuData();
}

void ZStatSampler::Sample(uint64_t value) const
{
    auto& data = *CpuLocal<CpuData>(CpuId());
    data.nsamples.fetch_add(1, std::memory_order_relaxed);
    data.sum.fetch_add(value, std::memory_order_relaxed);
    uint64_t maximum = data.max.load(std::memory_order_relaxed);
    while (maximum < value && !data.max.compare_exchange_weak(maximum, value, std::memory_order_relaxed)) {}
}

ZStatSamplerData ZStatSampler::CollectAndReset() const
{
    ZStatSamplerData result;
    for (size_t i = 0; i < CpuCount(); ++i) {
        auto& data = *CpuLocal<CpuData>(i);
        if (data.nsamples.load(std::memory_order_relaxed) != 0) {
            result.Add({data.nsamples.exchange(0, std::memory_order_relaxed),
                        data.sum.exchange(0, std::memory_order_relaxed),
                        data.max.exchange(0, std::memory_order_relaxed)});
        }
    }
    return result;
}

ZStatCounter::ZStatCounter(const char* group, const char* name, ZStatUnit unit)
    : ZStatValue(group, name, count++, sizeof(CpuData)), next(first), sampler(group, name, unit)
{
    first = this;
}

void ZStatCounter::Initialize() const
{
    for (size_t i = 0; i < CpuCount(); ++i) new (CpuLocal<CpuData>(i)) CpuData();
}

void ZStatCounter::Increment(uint64_t value) const
{
    CpuLocal<CpuData>(CpuId())->value.fetch_add(value, std::memory_order_relaxed);
}

void ZStatCounter::SampleAndReset() const
{
    uint64_t value = 0;
    for (size_t i = 0; i < CpuCount(); ++i) {
        value += CpuLocal<CpuData>(i)->value.exchange(0, std::memory_order_relaxed);
    }
    sampler.Sample(value);
}

void ZStat::Initialize()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        ZStatValue::InitializeStorage();
        ZStatSampler::Sort();
        for (const auto* sampler = ZStatSampler::First(); sampler != nullptr; sampler = sampler->Next()) {
            sampler->Initialize();
        }
        for (const auto* counter = ZStatCounter::First(); counter != nullptr; counter = counter->Next()) {
            counter->Initialize();
        }
    });
}

static void SampleAndCollect(std::vector<ZStatSamplerHistory>& history)
{
    for (const auto* counter = ZStatCounter::First(); counter != nullptr; counter = counter->Next()) {
        counter->SampleAndReset();
    }
    for (const auto* sampler = ZStatSampler::First(); sampler != nullptr; sampler = sampler->Next()) {
        history[sampler->Id()].Add(sampler->CollectAndReset());
    }
}

static void Print(const std::vector<ZStatSamplerHistory>& history)
{
    // zStat.cpp:1052-1064: logging controls output, never collection.
    if (Logger::GetLogger().GetMinimumLogLevel() > RTLOG_INFO) return;
    LOG(RTLOG_INFO, "GC Statistics: Last 10s / Last 10m / Last 10h / Total (average / maximum)");
    for (const auto* sampler = ZStatSampler::First(); sampler != nullptr; sampler = sampler->Next()) {
        const auto windows = history[sampler->Id()].Windows();
        const char* unit = "ns";
        switch (sampler->Unit()) {
            case ZStatUnit::TIME: unit = "ns"; break;
            case ZStatUnit::BYTES: unit = "B"; break;
            case ZStatUnit::THREADS: unit = "threads"; break;
            case ZStatUnit::BYTES_PER_SECOND: unit = "B/s"; break;
            case ZStatUnit::OPS_PER_SECOND: unit = "ops/s"; break;
        }
        LOG(RTLOG_INFO, "%s: %s %llu/%llu %llu/%llu %llu/%llu %llu/%llu %s",
            sampler->Group(), sampler->Name(),
            static_cast<unsigned long long>(windows[0].Average()), static_cast<unsigned long long>(windows[0].max),
            static_cast<unsigned long long>(windows[1].Average()), static_cast<unsigned long long>(windows[1].max),
            static_cast<unsigned long long>(windows[2].Average()), static_cast<unsigned long long>(windows[2].max),
            static_cast<unsigned long long>(windows[3].Average()), static_cast<unsigned long long>(windows[3].max), unit);
    }
}

// zStat.cpp:1022-1027
ZStat::ZStat()
{
    Initialize();
    set_name("ZStat");
    create_and_start();
}

// zStat.cpp:1093-1095: terminate wakes the sampling wait.
void ZStat::terminate()
{
    std::lock_guard<std::mutex> guard(lock);
    stopped = true;
    condition.notify_all();
}

// zStat.cpp:1070-1091
void ZStat::run_thread()
{
    std::vector<ZStatSamplerHistory> history(ZStatSampler::Count());
    const auto interval = std::chrono::seconds(1); // zStat.hpp: SampleHz = 1
    auto deadline = std::chrono::steady_clock::now() + interval;
    uint64_t ticks = 0;
    std::unique_lock<std::mutex> guard(lock);
    while (!condition.wait_until(guard, deadline, [this] { return stopped; })) {
        SampleAndCollect(history);
        if (++ticks % 10 == 0) Print(history); // ZStatisticsInterval default: 10 seconds
        do { deadline += interval; } while (deadline <= std::chrono::steady_clock::now());
    }
    Print(history);
}

void ZStat::EnterStwScope() { stwDepth.fetch_add(1, std::memory_order_relaxed); }
void ZStat::ExitStwScope() { stwDepth.fetch_sub(1, std::memory_order_relaxed); }
bool ZStat::WorldStoppedNow() { return stwDepth.load(std::memory_order_relaxed) != 0; }
} // namespace MapleRuntime

namespace MapleRuntime {
namespace ZStatPhases {
const ZStatPhase PCollectFromSpaceGarbage("Old Subphase", "CollectFromSpaceGarbage");
const ZStatPhase PCollectLargeGarbage("Old Subphase", "Collect large garbage");
const ZStatPhase PConcurrentMarking("Old Subphase", "Concurrent marking");
const ZStatPhase PConcurrentReMarking("Old Subphase", "Concurrent re-marking");
const ZStatPhase PConcurrentResurrection("Old Subphase", "concurrent resurrection");
const ZStatPhase PDoTracing("Old Subphase", "DoTracing");
const ZStatPhase PEnumRootsUpdateOldPointersWithin("Old Subphase", "enum roots & update old pointers within");
const ZStatPhase PExemptFromRegions("Old Subphase", "ExemptFromRegions");
const ZStatCriticalPhase PFinalizer("Finalizer");
const ZStatCriticalPhase PFinalizerProcessorWaittingTime("finalizerProcessor waitting time");
const ZStatPhase YoungForwardFromRegions("Young Subphase", "ForwardFromRegions");
const ZStatPhase OldForwardFromRegions("Old Subphase", "ForwardFromRegions");
const ZStatPhase PIdentifyUselessExternRef("Old Subphase", "identify useless extern ref");
const ZStatPhase POldRelocateStart("Old Pause", "old.relocate_start");
const ZStatPhase PPostTrace("Old Subphase", "PostTrace");
const ZStatPhase PPreforward("Old Subphase", "Preforward");
const ZStatCriticalPhase PReclaimGarbageRegions("ReclaimGarbageRegions");
const ZStatPhase PRemapYoungRoots("Old Subphase", "RemapYoungRoots");
const ZStatPhase PTraceLiveObjectsUpdateOldPointersInRefFields("Old Subphase", "trace live objects & update old pointers in ref-fields");
const ZStatPhase PYoungConcPromoteWalk("Young Subphase", "young.conc_promote_walk");
const ZStatPhase PYoungConcurrentRelocate("Young Subphase", "young.concurrent_relocate");
const ZStatPhase PYoungEvacFinish("Young Subphase", "young.evac_finish");
const ZStatPhase PYoungEvacRetire("Young Subphase", "young.evac_retire");
const ZStatPhase PYoungFlushAlloc("Young Subphase", "young.flush_alloc");
const ZStatPhase PYoungMarkClosure("Young Subphase", "young.mark_closure");
const ZStatPhase PYoungMarkFollow("Young Subphase", "young.mark_follow");
const ZStatPhase PYoungMarkFromRemset("Young Subphase", "young.mark_from_remset");
const ZStatPhase PYoungPinnedScan("Young Subphase", "young.pinned_scan");
const ZStatPhase PYoungPostEvacFinish("Young Subphase", "young.post_evac_finish");
const ZStatPhase PYoungPreEvacClear("Young Subphase", "young.pre_evac_clear");
const ZStatPhase PYoungPrepareCandidates("Young Subphase", "young.prepare_candidates");
const ZStatPhase PYoungRefFix("Young Subphase", "young.ref_fix");
const ZStatPhase PYoungRefFixBulk("Young Subphase", "young.ref_fix_bulk");
const ZStatPhase PYoungRefFixPrepare("Young Subphase", "young.ref_fix_prepare");
const ZStatPhase PYoungRefFixRootPass1("Young Subphase", "young.ref_fix_root_pass1");
const ZStatPhase PYoungRemsetDrain("Young Subphase", "young.remset_drain");
const ZStatPhase PYoungRemsetRescan("Young Subphase", "young.remset_rescan");
const ZStatPhase PYoungRootEnum("Young Subphase", "young.root_enum");
const ZStatPhase YoungGeneration("Young Generation", "Young Generation");
const ZStatPhase OldGeneration("Old Generation", "Old Generation");
const ZStatPhase MinorCollection("Minor Collection", "Minor Collection");
const ZStatPhase MajorCollection("Major Collection", "Major Collection");
} // namespace ZStatPhases
} // namespace MapleRuntime

#include "Base/AtomicSpinLock.h"
#include "Heap/Collector/TruncatedSeq.h"
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
namespace {
const ZStatCounter mutatorAllocated("Memory", "Allocation Rate", ZStatUnit::BYTES_PER_SECOND);
AtomicSpinLock g_statLock;
uint64_t g_lastSampleTimeNs = 0;
std::atomic<size_t> g_samplingGranule{ 1 };
std::atomic<size_t> g_allocatedSinceSample{ 0 };
TruncatedSeq g_samplesTime(100);
TruncatedSeq g_samplesBytes(100);
TruncatedSeq g_rate(100);
std::atomic<size_t> g_softMaxHeapSize{ 0 };
} // namespace

void ZStatMutatorAllocRate::update_sampling_granule()
{
    // zStat.cpp:951-955 — sampling_heap_granules = 128, align_up to granule size.
    constexpr size_t samplingHeapGranules = 128;
    size_t softMax = soft_max_heap_size();
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

void ZStatMutatorAllocRate::initialize()
{
    g_lastSampleTimeNs = TimeUtil::NanoSeconds();
    g_allocatedSinceSample.store(0, std::memory_order_relaxed);
    g_samplesTime.reset();
    g_samplesBytes.reset();
    g_rate.reset();
    // zDirector.cpp:867 / zHeap.cpp:61 — SoftMaxHeapSize. Env missing or 0 ⇒ hard cap.
    // ParseSizeFromEnv returns KB, same as cjHeapSize.
    size_t hard = Heap::GetHeap().GetMaxCapacity();
    size_t soft = hard;
    const char* env = std::getenv("cjSoftMaxHeapSize");
    if (env != nullptr) {
        const size_t parsedKb = CString::ParseSizeFromEnv(env);
        if (parsedKb > 0) {
            const size_t parsed = parsedKb * KB;
            if (hard == 0 || parsed <= hard) {
                soft = parsed;
            } else {
                soft = hard;
            }
        }
    }
    g_softMaxHeapSize.store(soft, std::memory_order_release);
    update_sampling_granule();
}

void ZStatMutatorAllocRate::sample_allocation(size_t allocationBytes)
{
    mutatorAllocated.Increment(allocationBytes);
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
    g_statLock.Unlock();
}

ZStatMutatorAllocRateStats ZStatMutatorAllocRate::stats()
{
    g_statLock.Lock();
    ZStatMutatorAllocRateStats out;
    out.avg = g_rate.avg();
    out.predict = g_rate.predict_next();
    out.sd = g_rate.sd();
    g_statLock.Unlock();
    return out;
}

size_t ZStatMutatorAllocRate::soft_max_heap_size()
{
    size_t soft = g_softMaxHeapSize.load(std::memory_order_acquire);
    if (soft != 0) {
        return soft;
    }
    return Heap::GetHeap().GetMaxCapacity();
}

} // namespace MapleRuntime

namespace MapleRuntime {
namespace {
ZStatHeap youngHeap("Young Generation");
ZStatHeap oldHeap("Old Generation");
}
ZStatHeap& ZStat::YoungHeap() { return youngHeap; }
ZStatHeap& ZStat::OldHeap() { return oldHeap; }
void ZStatHeap::AtRelocateEnd(size_t used, size_t live, size_t reclaimedBytes)
{
    std::lock_guard<std::mutex> guard(lock);
    stats.usedAtRelocateEnd = used;
    stats.liveAtMarkEnd = live;
    stats.reclaimedAverage = initialized ?
        stats.reclaimedAverage + 0.7 * (static_cast<double>(reclaimedBytes) - stats.reclaimedAverage) :
        static_cast<double>(reclaimedBytes);
    initialized = true;
    reclaimed.Sample(reclaimedBytes);
}
ZStatHeapStats ZStatHeap::Stats() const
{
    std::lock_guard<std::mutex> guard(lock);
    auto result = stats;
    result.reclaimedAverage += std::numeric_limits<double>::denorm_min();
    return result;
}
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void TracingCollector::UpdateGCStats()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& gcStats = GetGCStats();
    gcStats.Dump();

    size_t oldThreshold = gcStats.GetThreshold();
    size_t liveBytes = space.AllocatedBytes();
    size_t heapSize = space.GetMaxCapacity();
    size_t recentBytes = space.GetRecentAllocatedSize();

    // 2 / 3: when live bytes is over 2/3 heap size, the async allocation need to be closed.
    if (liveBytes > heapSize * 2 / 3) {
        space.EnableAsyncAllocation(false);
    } else {
        space.EnableAsyncAllocation(true);
    }
    // 4 ways to estimate heap next threshold.
#if defined (__OHOS__)
    constexpr double lowUtilGrowth = 1.8;
    constexpr double lowUtilRatio = 0.25;
    double heapGrowth = liveBytes < heapSize * lowUtilRatio ?
        lowUtilGrowth : 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#else
    double heapGrowth = 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#endif
    size_t threshold1 = static_cast<size_t>(liveBytes * heapGrowth);
    size_t threshold2 = static_cast<size_t>(oldThreshold * heapGrowth);
    size_t threshold3 = static_cast<size_t>(liveBytes * 1.2 / (1.0 + gcStats.garbageRatio));
    size_t threshold4 = space.GetTargetSize();
    size_t newThreshold = 0;
    uint64_t gcInterval = CangjieRuntime::GetGCParam().gcInterval;
    // 2 : We regard the half of heap size as a limit because of copying algorithm.
    if (liveBytes < oldThreshold && oldThreshold < (heapSize / 2)) {
#if defined (__OHOS__)
        // When the ulitization is low, we can give the old threshold a larger weight to compute average value.
        // 1, 4, 2, 1: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 1 + threshold2 * 4 + threshold3 * 2 + threshold4 * 1) / 8;
        // 2s: We set the max waiting time to 2s to avoid memory increasing too fast.
        auto maxAdaptiveInterval = static_cast<uint64_t>(2) * MapleRuntime::SECOND_TO_NANO_SECOND;
        uint64_t gcAdaptiveInterval = maxAdaptiveInterval;
        if (gcStats.collectionRate > 0.0) {
            double estimatedInterval = static_cast<double>(newThreshold - liveBytes) / MB /
                gcStats.collectionRate * MapleRuntime::SECOND_TO_NANO_SECOND;
            gcAdaptiveInterval = static_cast<uint64_t>(
                std::min(estimatedInterval, static_cast<double>(maxAdaptiveInterval)));
        }
        gcInterval = std::max(gcInterval, gcAdaptiveInterval);
#else
        // 4: Computing arithmetic mean
        newThreshold = (threshold1 + threshold2 + threshold3 + threshold4) / 4;
#endif
    } else {
        // When the ulitization is high, we try to avoid threshold increasing and give it a small weight.
        // 2, 1, 2, 3: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 2 + threshold2 * 1 + threshold3 * 2 + threshold4 * 3) / 8;
    }
    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(space.GetMaxCapacity() * 0.98));
    gcStats.heapThreshold.store(std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold),
                                std::memory_order_release);
    g_gcRequests[GC_REASON_HEU].SetMinInterval(gcInterval);
    VLOG(REPORT, "live bytes %zu (survived %zu, recent-allocated %zu), update gc threshold %zu -> %zu", liveBytes,
         liveBytes - recentBytes, recentBytes, oldThreshold, gcStats.GetThreshold());
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

namespace MapleRuntime {
void ZStatSamplerData::Add(const ZStatSamplerData& value)
    {
        nsamples += value.nsamples;
        sum += value.sum;
        max = std::max(max, value.max);
    }
}

namespace MapleRuntime {
void ZStatSamplerHistory::Add(const ZStatSamplerData& sample)
    {
        if (seconds.Add(sample) && minutes.Add(seconds.Total()) && hours.Add(minutes.Total())) {
            total.Add(hours.Total());
        }
    }
}

namespace MapleRuntime {
const char* ZStatValue::Group() const { return group; }
}

namespace MapleRuntime {
const char* ZStatValue::Name() const { return name; }
}

namespace MapleRuntime {
uint32_t ZStatValue::Id() const { return id; }
}




namespace MapleRuntime {
ZStatPhase::ZStatPhase(const char* group, const char* name) : sampler(group, name, ZStatUnit::TIME) {}
}

namespace MapleRuntime {
const char* ZStatPhase::Name() const { return sampler.Name(); }
}

namespace MapleRuntime {
void ZStatPhase::RegisterEnd(uint64_t duration) const { sampler.Sample(duration); }
}

namespace MapleRuntime {
ZStatCriticalPhase::ZStatCriticalPhase(const char* name)
        : ZStatPhase("Critical", name), counter("Critical", name, ZStatUnit::OPS_PER_SECOND) {}
}

namespace MapleRuntime {
void ZStatCriticalPhase::RegisterEnd(uint64_t duration) const {
        ZStatPhase::RegisterEnd(duration);
        counter.Increment();
    }
}

namespace MapleRuntime {
ZStatHeap::ZStatHeap(const char* group) : reclaimed(group, "Reclaimed", ZStatUnit::BYTES) {}
}
