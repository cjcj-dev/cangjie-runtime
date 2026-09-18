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
#include "Base/GcLog.h"
#include "Base/LogFile.h"
#include "CangjieRuntime.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zPageAllocator.hpp"
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

    // zStat.cpp:173-243 — the eight accessors.
    uint64_t avg_10_seconds() const { return seconds.Total().Average(); }
    uint64_t avg_10_minutes() const
    {
        const auto sum = minutes.Total().sum + seconds.Accumulated().sum;
        const auto n = minutes.Total().nsamples + seconds.Accumulated().nsamples;
        return n == 0 ? 0 : sum / n;
    }
    uint64_t avg_10_hours() const
    {
        const auto sum = hours.Total().sum + minutes.Accumulated().sum + seconds.Accumulated().sum;
        const auto n = hours.Total().nsamples + minutes.Accumulated().nsamples + seconds.Accumulated().nsamples;
        return n == 0 ? 0 : sum / n;
    }
    uint64_t avg_total() const
    {
        const auto sum = total.sum + hours.Accumulated().sum + minutes.Accumulated().sum + seconds.Accumulated().sum;
        const auto n = total.nsamples + hours.Accumulated().nsamples + minutes.Accumulated().nsamples +
                       seconds.Accumulated().nsamples;
        return n == 0 ? 0 : sum / n;
    }
    uint64_t max_10_seconds() const { return seconds.Total().max; }
    uint64_t max_10_minutes() const { return std::max(seconds.Accumulated().max, minutes.Total().max); }
    uint64_t max_10_hours() const
    {
        return std::max(std::max(seconds.Accumulated().max, minutes.Accumulated().max), hours.Total().max);
    }
    uint64_t max_total() const
    {
        return std::max(max_10_hours(), std::max(hours.Accumulated().max, total.max));
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
    parallelDuration = Sequence{};
    cycleIntervals = Sequence{};
    endOfLast = 0;
    hasEnded = false;
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
    // zStat.cpp:1242-1268
    const uint64_t previousEnd = hasEnded ? endOfLast : 0;
    end = now;
    endOfLast = now;
    hasEnded = true;
    if (warmup && warmupCycles < 3) {
        ++warmupCycles;
    }
    // Calculate serial and parallelizable GC cycle times
    const double duration = static_cast<double>(now - start) / SECOND_TO_NANO_SECOND;
    const double workersDuration = statWorkers->get_and_reset_duration();
    const double workersTime = statWorkers->get_and_reset_time();
    const double serialTime = duration - workersDuration;
    lastActiveWorkers = workersDuration > 0.0 ? workersTime / workersDuration : 1.0;
    if (recordStats) {
        serial.Add(serialTime);
        parallel.Add(workersTime);
        parallelDuration.Add(workersDuration);
        if (previousEnd != 0) {
            cycleIntervals.Add(static_cast<double>(now - previousEnd) / SECOND_TO_NANO_SECOND);
        }
    }
}

ZStatCycleStats ZStatCycle::Stats(uint64_t now) const
{
    std::lock_guard<std::mutex> guard(lock);
    ZStatCycleStats out;
    out.warmupCycles = warmupCycles;
    out.timeSinceLast = static_cast<double>(now - std::min(now, end)) / SECOND_TO_NANO_SECOND;
    out.serialTime = serial.average;
    out.serialTimeSd = std::sqrt(serial.variance);
    out.parallelTime = parallel.average;
    out.parallelTimeSd = std::sqrt(parallel.variance);
    out.lastActiveWorkers = lastActiveWorkers;
    out.durationSinceStart = static_cast<double>(now - start) / SECOND_TO_NANO_SECOND;
    out.isWarm = warmupCycles >= 3;
    out.isTimeTrustable = warmupCycles > 0;
    out.avgCycleInterval = cycleIntervals.average;
    out.parallelDuration = parallelDuration.average;
    out.parallelDurationSd = std::sqrt(parallelDuration.variance);
    return out;
}



} // namespace MapleRuntime


#include <chrono>
#include <cstdio>
#if defined(__linux__) || defined(hongmeng)
#include <sched.h>
#include <unistd.h>
#endif
#include "Base/LogFile.h"
#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zUtils.inline.hpp"

namespace MapleRuntime {
void ZTracer::report_stat_sampler(const ZStatSampler& sampler, uint64_t value)
{
    // zStat.cpp:133-152: route to Cangjie events once they exist (#626 D4-A).
    (void)sampler; (void)value;
}

void ZTracer::report_stat_counter(const ZStatValue& counter, uint64_t increment, uint64_t value)
{
    (void)counter; (void)increment; (void)value;
}

void ZTracer::report_stat_phase(const char* name, uint64_t durationNs)
{
    (void)name; (void)durationNs;
}

ZStatSampler* ZStatSampler::first = nullptr;
uint32_t ZStatSampler::count = 0;
ZStatCounter* ZStatCounter::first = nullptr;
uint32_t ZStatCounter::count = 0;
size_t ZStatValue::stride = 0;
char* ZStatValue::base = nullptr;

ZStatValue::ZStatValue(const char* group, const char* name, uint32_t id, size_t size)
    : group(group), name(name), id(id), offset(stride)
{
    MRT_ASSERT(base == nullptr, "statistics registered after initialization");
    stride += size;
}

// zStat.cpp:362-369: one cache-line aligned, unfreeable block of
// ZCPU::count() * stride bytes.
void ZStatValue::InitializeStorage()
{
    stride = AlignUp(stride, ZCacheLineSize);
    base = reinterpret_cast<char*>(ZUtils::alloc_aligned_unfreeable(ZCacheLineSize, stride * ZCPU::count()));
}

ZStatSampler::ZStatSampler(const char* group, const char* name, ZStatUnitPrinter printer)
    : ZStatValue(group, name, count++, sizeof(CpuData)), next(first), printer(printer)
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
    for (uint32_t i = 0; i < ZCPU::count(); ++i) new (CpuLocal<CpuData>(i)) CpuData();
}

ZStatSamplerData ZStatSampler::CollectAndReset() const
{
    ZStatSamplerData result;
    for (uint32_t i = 0; i < ZCPU::count(); ++i) {
        auto& data = *CpuLocal<CpuData>(i);
        if (data.nsamples.load(std::memory_order_relaxed) != 0) {
            result.Add({data.nsamples.exchange(0, std::memory_order_relaxed),
                        data.sum.exchange(0, std::memory_order_relaxed),
                        data.max.exchange(0, std::memory_order_relaxed)});
        }
    }
    return result;
}

void ZStatSampler::Sample(uint64_t value) const
{
    auto& data = *CpuLocal<CpuData>(ZCPU::id());
    data.nsamples.fetch_add(1, std::memory_order_relaxed);
    data.sum.fetch_add(value, std::memory_order_relaxed);
    uint64_t maximum = data.max.load(std::memory_order_relaxed);
    while (maximum < value && !data.max.compare_exchange_weak(maximum, value, std::memory_order_relaxed)) {}
}

void ZStatCounter::Increment(uint64_t value) const
{
    CpuLocal<CpuData>(ZCPU::id())->value.fetch_add(value, std::memory_order_relaxed);
}

ZStatCounter::ZStatCounter(const char* group, const char* name, ZStatUnitPrinter printer)
    : ZStatValue(group, name, count++, sizeof(CpuData)), next(first), sampler(group, name, printer)
{
    first = this;
}

void ZStatCounter::Initialize() const
{
    for (uint32_t i = 0; i < ZCPU::count(); ++i) new (CpuLocal<CpuData>(i)) CpuData();
}

void ZStatCounter::SampleAndReset() const
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < ZCPU::count(); ++i) {
        value += CpuLocal<CpuData>(i)->value.exchange(0, std::memory_order_relaxed);
    }
    ZStatSample(sampler, value);
}

ZStatUnsampledCounter* ZStatUnsampledCounter::first = nullptr;
uint32_t ZStatUnsampledCounter::count = 0;

ZStatUnsampledCounter::ZStatUnsampledCounter(const char* name)
    : ZStatValue("Unsampled", name, count++, sizeof(CpuData)), next(first)
{
    first = this;
}

ZStatCounterData* ZStatUnsampledCounter::Get() const
{
    return reinterpret_cast<ZStatCounterData*>(CpuLocal<CpuData>(ZCPU::id()));
}

ZStatCounterData ZStatUnsampledCounter::GetAndReset() const
{
    ZStatCounterData all;
    for (uint32_t i = 0; i < ZCPU::count(); ++i) {
        all.counter += CpuLocal<CpuData>(i)->value.exchange(0, std::memory_order_relaxed);
    }
    return all;
}

// zStat.cpp:892-930
void ZStatSample(const ZStatSampler& sampler, uint64_t value)
{
    ZStatSample(sampler, value);
    ZTracer::report_stat_sampler(sampler, value);
}

void ZStatDurationSample(const ZStatSampler& sampler, uint64_t durationNs)
{
    ZStatSample(sampler, durationNs);
}

void ZStatInc(const ZStatCounter& counter, uint64_t increment)
{
    counter.Increment(increment);
    ZTracer::report_stat_counter(counter, increment, 0);
}

void ZStatInc(const ZStatUnsampledCounter& counter, uint64_t increment)
{
    reinterpret_cast<ZStatUnsampledCounter::CpuData*>(counter.Get())->value.fetch_add(
        increment, std::memory_order_relaxed);
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
        sampler->Printer()(*sampler, history[sampler->Id()]);
    }
}

// zStat.cpp:246-335
static void PrintUnit(const ZStatSampler& sampler, const ZStatSamplerHistory& history, const char* unit)
{
    LOG(RTLOG_INFO, "%s: %s %llu/%llu %llu/%llu %llu/%llu %llu/%llu %s",
        sampler.Group(), sampler.Name(),
        static_cast<unsigned long long>(history.avg_10_seconds()),
        static_cast<unsigned long long>(history.max_10_seconds()),
        static_cast<unsigned long long>(history.avg_10_minutes()),
        static_cast<unsigned long long>(history.max_10_minutes()),
        static_cast<unsigned long long>(history.avg_10_hours()),
        static_cast<unsigned long long>(history.max_10_hours()),
        static_cast<unsigned long long>(history.avg_total()),
        static_cast<unsigned long long>(history.max_total()), unit);
}

void ZStatUnitTimeNs(const ZStatSampler& sampler, const ZStatSamplerHistory& history)
{
    PrintUnit(sampler, history, "ns");
}

void ZStatUnitBytes(const ZStatSampler& sampler, const ZStatSamplerHistory& history)
{
    PrintUnit(sampler, history, "B");
}

void ZStatUnitBytesPerSecond(const ZStatSampler& sampler, const ZStatSamplerHistory& history)
{
    PrintUnit(sampler, history, "B/s");
}

void ZStatUnitCount(const ZStatSampler& sampler, const ZStatSamplerHistory& history)
{
    PrintUnit(sampler, history, "threads");
}

void ZStatUnitOpsPerSecond(const ZStatSampler& sampler, const ZStatSamplerHistory& history)
{
    PrintUnit(sampler, history, "ops/s");
}

// zStat.cpp:1022-1027
ZStat::ZStat() : metronome(SampleHz)
{
    Initialize();
    set_name("ZStat");
    create_and_start();
}

// zStat.cpp:1093-1095: terminate wakes the sampling wait.
void ZStat::terminate()
{
    metronome.stop();
}

// zStat.cpp:1070-1091
void ZStat::run_thread()
{
    std::vector<ZStatSamplerHistory> history(ZStatSampler::Count());
    uint64_t ticks = 0;
    while (metronome.wait_for_tick()) {
        SampleAndCollect(history);
        if (++ticks % 10 == 0) Print(history); // ZStatisticsInterval default: 10 seconds
    }
    Print(history);
}

} // namespace MapleRuntime

namespace MapleRuntime {
} // namespace MapleRuntime

#include "Base/AtomicSpinLock.h"
#include "Base/TruncatedSeq.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
namespace {
const ZStatCounter mutatorAllocated("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
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
    const size_t unit = ZPage::UNIT_SIZE == 0 ? 4096 : ZPage::UNIT_SIZE;
    granule = AlignUp(granule, unit);
    if (granule == 0) {
        granule = unit;
    }
    g_samplingGranule.store(granule, std::memory_order_release);
}

void ZStatMutatorAllocRate::initialize()
{
    // zStat.cpp:945-949: the sample windows are process-lifetime statics;
    // initialize only stamps the last sample time and the granule.
    g_lastSampleTimeNs = TimeUtil::NanoSeconds();
    g_allocatedSinceSample.store(0, std::memory_order_relaxed);
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
    ZStatInc(mutatorAllocated, allocationBytes);
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

    // zStat.cpp:1008 — rule evaluation is triggered at the end of every
    // allocation-rate sample, not only on the director's own tick.
    ZDirector::evaluate_rules();
}

static const ZStatUnsampledCounter mutatorAllocRateCounter("Allocation Rate");

const ZStatUnsampledCounter& ZStatMutatorAllocRate::counter()
{
    return mutatorAllocRateCounter;
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
void ZStatHeap::AtCollectionStart(size_t used)
{
    std::lock_guard<std::mutex> guard(lock);
    usedAtCollectionStart = used;
    lastReclaimed = 0;
}

void ZStatHeap::AtMarkEnd(size_t live)
{
    std::lock_guard<std::mutex> guard(lock);
    stats.liveAtMarkEnd = live;
}

void ZStatHeap::AddReclaimed(size_t bytes)
{
    std::lock_guard<std::mutex> guard(lock);
    lastReclaimed += bytes;
}

void ZStatHeap::AtRelocateEnd(size_t used, size_t live, size_t reclaimedBytes)
{
    std::lock_guard<std::mutex> guard(lock);
    stats.usedAtRelocateEnd = used;
    stats.liveAtMarkEnd = live;
    lastReclaimed = reclaimedBytes;
    stats.reclaimedAverage = initialized ?
        stats.reclaimedAverage + 0.7 * (static_cast<double>(reclaimedBytes) - stats.reclaimedAverage) :
        static_cast<double>(reclaimedBytes);
    initialized = true;
    ZStatSample(reclaimed, reclaimedBytes);
}

size_t ZStatHeap::UsedAtCollectionStart() const
{
    std::lock_guard<std::mutex> guard(lock);
    return usedAtCollectionStart;
}

size_t ZStatHeap::LiveAtMarkEnd() const
{
    std::lock_guard<std::mutex> guard(lock);
    return stats.liveAtMarkEnd;
}

size_t ZStatHeap::UsedAtRelocateEnd() const
{
    std::lock_guard<std::mutex> guard(lock);
    return stats.usedAtRelocateEnd;
}

size_t ZStatHeap::LastReclaimed() const
{
    std::lock_guard<std::mutex> guard(lock);
    return lastReclaimed;
}

double ZStatHeap::ReclaimedAvg()
{
    std::lock_guard<std::mutex> guard(lock);
    return stats.reclaimedAverage + std::numeric_limits<double>::denorm_min();
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
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"




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
ZStatPhase::ZStatPhase(const char* group, const char* name) : sampler(group, name, ZStatUnitTimeNs) {}
}

// zStat.cpp:513-591
namespace MapleRuntime {
ZStatMMUPause::ZStatMMUPause() : start(0.0), end(0.0) {}

ZStatMMUPause::ZStatMMUPause(uint64_t startNs, uint64_t endNs)
    : start(static_cast<double>(startNs) / MILLI_SECOND_TO_NANO_SECOND),
      end(static_cast<double>(endNs) / MILLI_SECOND_TO_NANO_SECOND)
{}

double ZStatMMUPause::End() const { return end; }

double ZStatMMUPause::Overlap(double startMs, double endMs) const
{
    const double startMax = std::max(startMs, start);
    const double endMin = std::min(endMs, end);
    if (endMin > startMax) {
        // Overlap found
        return endMin - startMax;
    }
    // No overlap
    return 0.0;
}

size_t ZStatMMU::next = 0;
size_t ZStatMMU::npauses = 0;
ZStatMMUPause ZStatMMU::pauses[ZStatMMU::RingSize];
double ZStatMMU::mmu2ms = 100.0;
double ZStatMMU::mmu5ms = 100.0;
double ZStatMMU::mmu10ms = 100.0;
double ZStatMMU::mmu20ms = 100.0;
double ZStatMMU::mmu50ms = 100.0;
double ZStatMMU::mmu100ms = 100.0;

const ZStatMMUPause& ZStatMMU::PauseAt(size_t index)
{
    return pauses[(next - index - 1) % RingSize];
}

double ZStatMMU::CalculateMMU(double timeSliceMs)
{
    const double end = PauseAt(0).End();
    const double start = end - timeSliceMs;
    double timePaused = 0.0;

    // Find all overlapping pauses
    for (size_t i = 0; i < npauses; i++) {
        const double overlap = PauseAt(i).Overlap(start, end);
        if (overlap == 0.0) {
            // No overlap
            break;
        }
        timePaused += overlap;
    }

    // Calculate MMU
    const double timeMutator = timeSliceMs - timePaused;
    return timeMutator / timeSliceMs * 100.0;
}

void ZStatMMU::RegisterPause(uint64_t startNs, uint64_t endNs)
{
    // Add pause
    const size_t index = next++ % RingSize;
    pauses[index] = ZStatMMUPause(startNs, endNs);
    npauses = std::min(npauses + 1, RingSize);

    // Recalculate MMUs
    mmu2ms = std::min(mmu2ms, CalculateMMU(2));
    mmu5ms = std::min(mmu5ms, CalculateMMU(5));
    mmu10ms = std::min(mmu10ms, CalculateMMU(10));
    mmu20ms = std::min(mmu20ms, CalculateMMU(20));
    mmu50ms = std::min(mmu50ms, CalculateMMU(50));
    mmu100ms = std::min(mmu100ms, CalculateMMU(100));
}

void ZStatMMU::Print()
{
    LOG(RTLOG_INFO, "MMU: 2ms/%.1f%%, 5ms/%.1f%%, 10ms/%.1f%%, 20ms/%.1f%%, 50ms/%.1f%%, 100ms/%.1f%%",
        mmu2ms, mmu5ms, mmu10ms, mmu20ms, mmu50ms, mmu100ms);
}

// zStat.cpp:1410-1420
void ZStatLoad::Print()
{
#if defined(__linux__) || defined(hongmeng)
    double loadavg[3] = {};
    if (getloadavg(loadavg, 3) != -1) {
        const double cpus = static_cast<double>(ZCPU::count());
        LOG(RTLOG_INFO, "Load: %.2f (%.0f%%) / %.2f (%.0f%%) / %.2f (%.0f%%)",
            loadavg[0], loadavg[0] / cpus * 100.0,
            loadavg[1], loadavg[1] / cpus * 100.0,
            loadavg[2], loadavg[2] / cpus * 100.0);
    }
#endif
}

// zStat.cpp:1423-1456
ZStatMark::ZStatMark()
    : _nstripes(0), _nproactiveflush(0), _nterminateflush(0), _ntrycomplete(0), _ncontinue(0), _markStackUsage(0)
{}

void ZStatMark::AtMarkStart(size_t nstripes)
{
    _nstripes = nstripes;
}

void ZStatMark::AtMarkEnd(size_t nproactiveflush, size_t nterminateflush, size_t ntrycomplete, size_t ncontinue)
{
    _nproactiveflush = nproactiveflush;
    _nterminateflush = nterminateflush;
    _ntrycomplete = ntrycomplete;
    _ncontinue = ncontinue;
}

void ZStatMark::Print()
{
    LOG(RTLOG_INFO,
        "Mark: %zu stripe(s), %zu proactive flush(es), %zu terminate flush(es), "
        "%zu completion(s), %zu continuation(s) ",
        _nstripes, _nproactiveflush, _nterminateflush, _ntrycomplete, _ncontinue);
}

// zStat.cpp:1642-1697
ZStatReferences::ZCount ZStatReferences::soft;
ZStatReferences::ZCount ZStatReferences::weak;
ZStatReferences::ZCount ZStatReferences::final;
ZStatReferences::ZCount ZStatReferences::phantom;

void ZStatReferences::Set(ZCount* count, size_t encountered, size_t discovered, size_t enqueued)
{
    count->encountered = encountered;
    count->discovered = discovered;
    count->enqueued = enqueued;
}

void ZStatReferences::set_soft(size_t encountered, size_t discovered, size_t enqueued)
{
    Set(&soft, encountered, discovered, enqueued);
}

void ZStatReferences::set_weak(size_t encountered, size_t discovered, size_t enqueued)
{
    Set(&weak, encountered, discovered, enqueued);
}

void ZStatReferences::set_final(size_t encountered, size_t discovered, size_t enqueued)
{
    Set(&final, encountered, discovered, enqueued);
}

void ZStatReferences::set_phantom(size_t encountered, size_t discovered, size_t enqueued)
{
    Set(&phantom, encountered, discovered, enqueued);
}

void ZStatReferences::Print()
{
    LOG(RTLOG_INFO, "%-20s %12s %12s %12s", "References:", "Encountered", "Discovered", "Enqueued");
    auto printRow = [](const char* name, const ZCount& ref) {
        LOG(RTLOG_INFO, "%-20s %12zu %12zu %12zu", name, ref.encountered, ref.discovered, ref.enqueued);
    };
    printRow("Soft", soft);
    printRow("Weak", weak);
    printRow("Final", final);
    printRow("Phantom", phantom);
}
} // namespace MapleRuntime

// zStat.cpp:597-876 — stat phases. Host infra difference (D4=A): the
// ConcurrentGCTimer/JFR calls of ZGC's register_start/register_end have no
// counterpart; the structured record goes to GCLOG from the same routing
// point (ZTracer::report_stat_phase ≈ GcLog::Phase).
namespace MapleRuntime {
static void EmitPhaseRecord(const ZStatPhase& phase, const char* kind, uint64_t startNs, uint64_t endNs)
{
    GcLog::Phase(GcLog::CurrentSeq(), phase.Name(), kind, startNs, endNs - startNs);
}

const char* ZStatPhase::Name() const { return sampler.Name(); }

ZStatPhaseCollection::ZStatPhaseCollection(const char* name, bool minor)
    : ZStatPhase(minor ? "Minor Collection" : "Major Collection", name), minor(minor)
{}

// zStat.cpp:655-687 — the abort early-exit keeps an aborted cycle out of
// every downstream statistic.
void ZStatPhaseCollection::RegisterStart(uint64_t startNs) const { (void)startNs; }

void ZStatPhaseCollection::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    if (ZAbort::should_abort()) {
        return;
    }
    // rec=cycle is the collection-level structured record; rec=phase covers
    // pause/concurrent/subphase/critical work (same population the retired
    // Timer observed).
    ZStatDurationSample(sampler, endNs - startNs);
}

ZStatPhaseGeneration::ZStatPhaseGeneration(const char* name, ZGenerationId id)
    : ZStatPhase(id == ZGenerationId::old ? "Old Generation" : "Young Generation", name), id(id)
{}

void ZStatPhaseGeneration::RegisterStart(uint64_t startNs) const { (void)startNs; }

// zStat.cpp:711-759 — the per-collection report is printed once from here;
// the stalls/Load/Mark/References/relocation/heap units are added as those
// stat units land (later commits on this branch).
void ZStatPhaseGeneration::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    if (ZAbort::should_abort()) {
        return;
    }
    ZStatDurationSample(sampler, endNs - startNs);
    // zStat.cpp:724-735 — the one-shot per-collection report. The stalls,
    // mark, relocation and heap units join as their per-generation stat
    // objects land on this branch.
    ZStatLoad::Print();
    ZStatMMU::Print();
    if (id == ZGenerationId::old) {
        ZStatReferences::Print();
    }
}

uint64_t ZStatPhasePause::maxNs;

ZStatPhasePause::ZStatPhasePause(const char* name, ZGenerationId id)
    : ZStatPhase(id == ZGenerationId::young ? "Young Pause" : "Old Pause", name)
{}

uint64_t ZStatPhasePause::Max() { return maxNs; }

void ZStatPhasePause::RegisterStart(uint64_t startNs) const { (void)startNs; }

// zStat.cpp:766-797 — pauses feed the duration sampler, the max-pause
// tracker and the MMU ring; RegisterPause is the only MMU writer.
void ZStatPhasePause::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    const uint64_t duration = endNs - startNs;
    ZStatDurationSample(sampler, duration);

    // Track max pause time
    if (maxNs < duration) {
        maxNs = duration;
    }

    // Track minimum mutator utilization
    ZStatMMU::RegisterPause(startNs, endNs);

    EmitPhaseRecord(*this, "pause", startNs, endNs);
}

ZStatPhaseConcurrent::ZStatPhaseConcurrent(const char* name, ZGenerationId id)
    : ZStatPhase(id == ZGenerationId::young ? "Young Phase" : "Old Phase", name)
{}

void ZStatPhaseConcurrent::RegisterStart(uint64_t startNs) const { (void)startNs; }

void ZStatPhaseConcurrent::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    if (ZAbort::should_abort()) {
        return;
    }
    ZStatDurationSample(sampler, endNs - startNs);
    EmitPhaseRecord(*this, "conc", startNs, endNs);
}

ZStatSubPhase::ZStatSubPhase(const char* name, ZGenerationId id)
    : ZStatPhase(id == ZGenerationId::young ? "Young Subphase" : "Old Subphase", name)
{}

void ZStatSubPhase::RegisterStart(uint64_t startNs) const { (void)startNs; }

// zStat.cpp:826-846 — ZTracer::report_thread_phase routes here; on this host
// the thread-phase datum is the GCLOG phase record.
void ZStatSubPhase::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    if (ZAbort::should_abort()) {
        return;
    }
    ZStatDurationSample(sampler, endNs - startNs);
    EmitPhaseRecord(*this, "conc", startNs, endNs);
}

ZStatCriticalPhase::ZStatCriticalPhase(const char* name, bool verbose)
    : ZStatPhase("Critical", name), counter("Critical", name, ZStatUnitOpsPerSecond), verbose(verbose)
{}

void ZStatCriticalPhase::RegisterStart(uint64_t startNs) const
{
    // This is called from sensitive contexts, for example before an
    // allocation stall has been resolved. Nothing useful can be logged here.
    (void)startNs;
}

// zStat.cpp:862-876
void ZStatCriticalPhase::RegisterEnd(uint64_t startNs, uint64_t endNs) const
{
    ZStatDurationSample(sampler, endNs - startNs);
    ZStatInc(counter, 1);
    EmitPhaseRecord(*this, "conc", startNs, endNs);
}
} // namespace MapleRuntime

namespace MapleRuntime {
ZStatHeap::ZStatHeap(const char* group) : reclaimed(group, "Reclaimed", ZStatUnitBytes) {}
}

namespace MapleRuntime {
std::atomic<uint64_t> g_gcTotalTimeUs{ 0 };
std::atomic<size_t> g_gcCollectedTotalBytes{ 0 };
std::atomic<uint64_t> ZStat::prevGcStartTime{ TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS };
std::atomic<uint64_t> ZStat::prevGcFinishTime{ TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS };
}
