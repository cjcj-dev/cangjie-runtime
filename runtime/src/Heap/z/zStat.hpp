// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZSTAT_H
#define MRT_ZSTAT_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <array>
#include <memory>
#include <condition_variable>
#include <thread>
#include <algorithm>
#include <vector>

#include "Base/TimeUtils.h"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zMetronome.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zThread.hpp"

namespace MapleRuntime {
struct YoungConcWindowStats {
    uint64_t windowNs = 0;    // world-released → STW2 requested
    size_t closureCalls = 0;  // TraceYoungClosure invocations inside the window
    size_t markedAtEntry = 0; // reachableVec.size() at world-release
    size_t markedAtExit = 0;  // reachableVec.size() at STW2 request
    size_t remsetSlots = 0;   // remset slots consumed by the in-window rescan
    size_t reenters = 0;      // ZGC pause_mark_end() == false → concurrent_mark_continue()
    size_t MarkedInWindow() const { return markedAtExit >= markedAtEntry ? markedAtExit - markedAtEntry : 0; }
};

// zStat.hpp:449-452
struct ZStatWorkersStats {
    double _accumulated_time;
    double _accumulated_duration;
};

// zStat.hpp:454-479, zStat.cpp:1330-1405. Driven by ZWorkers::run before and
// after every task; stats() also counts the batch that is still running.
class ZStatWorkers {
private:
    std::mutex _stat_lock;
    uint32_t _active_workers;
    uint64_t _start_of_last;
    uint64_t _accumulated_duration;
    uint64_t _accumulated_time;

    double accumulated_duration();
    double accumulated_time();
    uint32_t active_workers();

public:
    ZStatWorkers();

    void at_start(uint32_t active_workers);
    void at_end();

    double get_and_reset_duration();
    double get_and_reset_time();

    ZStatWorkersStats stats();
};

// zStat.cpp:1226-1330: cycle inputs used by the director are always
// collected independently of log output.
struct ZStatCycleStats {
    uint32_t warmupCycles = 0;
    double timeSinceLast = 0;
    double serialTime = 0;
    double serialTimeSd = 0;
    double parallelTime = 0;
    double parallelTimeSd = 0;
    double lastActiveWorkers = 1;
    double durationSinceStart = 0;
    // zStat.hpp:403-417 — the remaining ZGC cycle statistics.
    bool isWarm = false;
    bool isTimeTrustable = false;
    double avgCycleInterval = 0;
    double parallelDuration = 0;
    double parallelDurationSd = 0;
};

// utilities/numberSeq.cpp:35-49,79-94: exponentially decaying mean
// and variance, alpha=0.7 as used by ZStatCycle and ZStatHeap.
struct ZStatNumberSeq {
    void Add(double value);
    double Average() const { return average; }
    double Sd() const;
    bool initialized = false;
    double average = 0;
    double variance = 0;
};

class ZStatCycle {
public:
    void Initialize(uint64_t now);
    // zStat.cpp:1237-1268: the parallel share comes from ZStatWorkers'
    // get_and_reset pair at the end of the cycle; record_stats gates the
    // sequences, never the reset.
    void AtStart(uint64_t now);
    void AtEnd(uint64_t now, ZStatWorkers* statWorkers, bool warmup, bool recordStats);
    ZStatCycleStats Stats(uint64_t now) const;

private:
    using Sequence = ZStatNumberSeq;
    mutable std::mutex lock;
    uint64_t start = 0;
    uint64_t end = 0;
    uint32_t warmupCycles = 0;
    double lastActiveWorkers = 1;
    Sequence serial;
    Sequence parallel;
    Sequence parallelDuration;
    Sequence cycleIntervals;
    uint64_t endOfLast = 0;
    bool hasEnded = false;
};

struct ZStatSamplerData;
struct ZStatSamplerHistory;
class ZStatValue;
class ZStatSampler;

// zStat.hpp:77-84
struct ZStatCounterData {
    uint64_t counter = 0;
    void Add(const ZStatCounterData& other) { counter += other.counter; }
};

// zStat.hpp:255: print format is a property of the sampler, selected at
// construction. The five printers live in zStat.cpp with ZStatSamplerHistory.
typedef void (*ZStatUnitPrinter)(const ZStatSampler& sampler, const ZStatSamplerHistory& history);

// zStat.cpp:246-335 — the five printers.
void ZStatUnitTimeNs(const ZStatSampler& sampler, const ZStatSamplerHistory& history);
void ZStatUnitBytes(const ZStatSampler& sampler, const ZStatSamplerHistory& history);
void ZStatUnitBytesPerSecond(const ZStatSampler& sampler, const ZStatSamplerHistory& history);
void ZStatUnitCount(const ZStatSampler& sampler, const ZStatSamplerHistory& history);
void ZStatUnitOpsPerSecond(const ZStatSampler& sampler, const ZStatSamplerHistory& history);

// zStat.hpp:124-141, zStat.cpp:133-152: tracer routing point. Cangjie events
// are not wired yet (issue #626 D4: infra difference A); every
// ZStatSample/ZStatInc passes through these stubs so the routing points exist.
class ZTracer {
public:
    static void report_stat_sampler(const ZStatSampler& sampler, uint64_t value);
    static void report_stat_counter(const ZStatValue& counter, uint64_t increment, uint64_t value);
    static void report_stat_phase(const char* name, uint64_t durationNs);
};

// Identity and list membership are fixed by static construction, before startup.
class ZStatValue {
public:
    static void initialize() { InitializeStorage(); }
    static bool StorageReadyPublic();
    const char* Group() const;
    const char* Name() const;
    uint32_t Id() const;
    ZStatValue(const ZStatValue&) = delete;
    ZStatValue& operator=(const ZStatValue&) = delete;
protected:
    ZStatValue(const char* group, const char* name, uint32_t id, size_t size);
    // Host infra difference: product entry points (e.g. the livemap
    // contention counters) can be reached before ZStat::Initialize in
    // embedding/test contexts. Samples taken before the per-CPU block
    // exists are discarded rather than touching unallocated storage.
    static bool StorageReady() { return base != nullptr; }
    template<typename T> T* CpuLocal(size_t cpu) const
    {
        return reinterpret_cast<T*>(base + stride * cpu + offset);
    }
    static void InitializeStorage();
    friend class ZStat;
private:
    const char* const group;
    const char* const name;
    const uint32_t id;
protected:
    const size_t offset;
private:
    static size_t stride;
    static char* base;
};

// zStat.hpp:252-277. Sampling goes through the free ZStatSample (with the
// ZTracer routing point), matching zStat.cpp:892-916.
class ZStatSampler : public ZStatValue {
public:
    ZStatSampler(const char* group, const char* name, ZStatUnitPrinter printer);
    void Initialize() const;
    ZStatSamplerData CollectAndReset() const;
    friend void ZStatSample(const ZStatSampler& sampler, uint64_t value);
    ZStatUnitPrinter Printer() const { return printer; }
    static ZStatSampler* First() { return first; }
    const ZStatSampler* Next() const { return next; }
    static uint32_t Count() { return count; }
    static void Sort();
private:
    struct alignas(64) CpuData {
        std::atomic<uint64_t> nsamples {0};
        std::atomic<uint64_t> sum {0};
        std::atomic<uint64_t> max {0};
    };
    static ZStatSampler* first;
    static uint32_t count;
    // zStat.cpp:405-428 sorts registry links even in const phase/counter samplers.
    // Keep that link writable when constant initialization places its owner in RELRO.
    mutable ZStatSampler* next;
    const ZStatUnitPrinter printer;
    void Sample(uint64_t value) const;
};

// zStat.hpp:285-303, zStat.cpp:460-487
class ZStatCounter : public ZStatValue {
public:
    ZStatCounter(const char* group, const char* name, ZStatUnitPrinter printer);
    void Initialize() const;
    void SampleAndReset() const;
    friend void ZStatInc(const ZStatCounter& counter, uint64_t increment);
    static ZStatCounter* First() { return first; }
    const ZStatCounter* Next() const { return next; }
    const ZStatSampler& Sampler() const { return sampler; }
private:
    struct alignas(64) CpuData { std::atomic<uint64_t> value {0}; };
    static ZStatCounter* first;
    static uint32_t count;
    ZStatCounter* const next;
    const ZStatSampler sampler;
    void Increment(uint64_t value) const;
};

// zStat.hpp:305-320, zStat.cpp:489-511: sampled into nothing; read directly.
class ZStatUnsampledCounter : public ZStatValue {
public:
    ZStatUnsampledCounter(const char* name);
    ZStatCounterData* Get() const;
    ZStatCounterData GetAndReset() const;
    friend void ZStatInc(const ZStatUnsampledCounter& counter, uint64_t increment);
    static ZStatUnsampledCounter* First() { return first; }
    const ZStatUnsampledCounter* Next() const { return next; }
private:
    struct alignas(64) CpuData { std::atomic<uint64_t> value {0}; };
    static ZStatUnsampledCounter* first;
    static uint32_t count;
    ZStatUnsampledCounter* const next;
};

// zStat.cpp:892-930 — every sample/inc passes the ZTracer routing point.
void ZStatSample(const ZStatSampler& sampler, uint64_t value);
void ZStatDurationSample(const ZStatSampler& sampler, uint64_t durationNs);
void ZStatInc(const ZStatCounter& counter, uint64_t increment);
void ZStatInc(const ZStatUnsampledCounter& counter, uint64_t increment);

// zStat.hpp:174-207, zStat.cpp:513-591: Minimum Mutator Utilization over a
// ring of the last 200 pauses, tracked at six window sizes. Host infra
// difference: Ticks has no counterpart; pauses are registered in nanoseconds
// and stored as milliseconds (double), the same datum as ZGC.
class ZStatMMUPause {
public:
    ZStatMMUPause();
    ZStatMMUPause(uint64_t startNs, uint64_t endNs);
    double End() const;
    double Overlap(double startMs, double endMs) const;
private:
    double start;
    double end;
};

class ZStatMMU {
public:
    static void RegisterPause(uint64_t startNs, uint64_t endNs);
    static void Print();
private:
    static constexpr size_t RingSize = 200; // Record the last 200 pauses
    static size_t next;
    static size_t npauses;
    static ZStatMMUPause pauses[RingSize];
    static double mmu2ms;
    static double mmu5ms;
    static double mmu10ms;
    static double mmu20ms;
    static double mmu50ms;
    static double mmu100ms;

    static const ZStatMMUPause& PauseAt(size_t index);
    static double CalculateMMU(double timeSliceMs);
};

// zStat.hpp:212-296, zStat.cpp:597-876: phase group and generation are
// properties of the static phase object; neither the observed name nor a
// cycle table owns it. Host infra difference (D4=A): ConcurrentGCTimer and
// the JFR tracers have no counterpart, so register_start/register_end carry
// no timer parameter; the structured phase record is emitted to GCLOG from
// the same routing point (ZTracer::report_stat_phase ≈ GcLog::Phase).
class ZStatPhase {
protected:
    const ZStatSampler sampler;

    ZStatPhase(const char* group, const char* name);

public:
    const char* Name() const;

    virtual void RegisterStart(uint64_t startNs) const = 0;
    virtual void RegisterEnd(uint64_t startNs, uint64_t endNs) const = 0;
    virtual ~ZStatPhase() = default;
    const ZStatSampler& Sampler() const { return sampler; }
};

// zStat.hpp:228-242
class ZStatPhaseCollection : public ZStatPhase {
public:
    ZStatPhaseCollection(const char* name, bool minor);

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;

private:
    const bool minor;
};

// zStat.hpp:244-255
class ZStatPhaseGeneration : public ZStatPhase {
public:
    ZStatPhaseGeneration(const char* name, ZGenerationId id);

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;

private:
    const ZGenerationId id;
};

// zStat.hpp:257-268
class ZStatPhasePause : public ZStatPhase {
public:
    ZStatPhasePause(const char* name, ZGenerationId id);

    static uint64_t Max();

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;

private:
    static uint64_t maxNs; // Max pause time
};

// zStat.hpp:270-276
class ZStatPhaseConcurrent : public ZStatPhase {
public:
    ZStatPhaseConcurrent(const char* name, ZGenerationId id);

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;
};

// zStat.hpp:278-284
class ZStatSubPhase : public ZStatPhase {
public:
    ZStatSubPhase(const char* name, ZGenerationId id);

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;
};

// zStat.hpp:286-296: critical phases register both duration and frequency.
class ZStatCriticalPhase : public ZStatPhase {
public:
    explicit ZStatCriticalPhase(const char* name, bool verbose = true);

    void RegisterStart(uint64_t startNs) const override;
    void RegisterEnd(uint64_t startNs, uint64_t endNs) const override;

private:
    const ZStatCounter counter;
    const bool verbose;
};

// zStat.hpp:301-342. The Young/Old variants select the generation's timer in
// ZGC; with no ConcurrentGCTimer on this host all three variants share the
// one RAII body (zStat.cpp:878-886).
class ZStatTimer {
public:
    explicit ZStatTimer(const ZStatPhase& phase) : phase(phase), start(TimeUtil::NanoSeconds())
    {
        phase.RegisterStart(start);
    }
    ~ZStatTimer()
    {
        phase.RegisterEnd(start, TimeUtil::NanoSeconds());
    }
    ZStatTimer(const ZStatTimer&) = delete;
    ZStatTimer& operator=(const ZStatTimer&) = delete;

private:
    const ZStatPhase& phase;
    const uint64_t start;
};

class ZStatTimerYoung : public ZStatTimer {
public:
    explicit ZStatTimerYoung(const ZStatPhase& phase) : ZStatTimer(phase) {}
};

class ZStatTimerOld : public ZStatTimer {
public:
    explicit ZStatTimerOld(const ZStatPhase& phase) : ZStatTimer(phase) {}
};

class ZStatTimerWorker : public ZStatTimer {
public:
    explicit ZStatTimerWorker(const ZStatPhase& phase) : ZStatTimer(phase) {}
};

// zStat.cpp:935-1017 — ZStatMutatorAllocRate.
struct ZStatMutatorAllocRateStats {
    double avg = 0.0;
    double predict = 0.0;
    double sd = 0.0;
};

class ZStatMutatorAllocRate {
public:
    static void initialize();
    static void sample_allocation(size_t allocationBytes);
    static ZStatMutatorAllocRateStats stats();

    // zStat.hpp:372-375: unsampled byte counter incremented at the allocation site.
    static const ZStatUnsampledCounter& counter();
    // zDirector.cpp:867 / zHeap.cpp:61 — SoftMaxHeapSize. Trigger denominator
    // only; allocation failure still uses hard capacity.

private:
    static void update_sampling_granule();
};


// zStatHeap (zStat.hpp:596-707): one synchronized heap account per
// generation, fed by ZPageAllocatorStats at the six sample points
// (zGeneration.cpp:382,883,910,928,938 and the old-generation counterparts).
class ZPageAllocatorStats;
class ZGeneration;

struct ZStatHeapStats {
    size_t usedAtRelocateEnd = 0;
    size_t liveAtMarkEnd = 0;
    double reclaimedAverage = 0;
};

class ZStatHeap {
public:
    ZStatHeap();

    void AtInitialize(size_t minCapacity, size_t maxCapacity);
    void AtCollectionStart(const ZPageAllocatorStats& stats);
    void AtMarkStart(const ZPageAllocatorStats& stats);
    void AtMarkEnd(const ZPageAllocatorStats& stats);
    void AtSelectRelocationSet(const ZRelocationSetSelectorStats& stats);
    void AtRelocateStart(const ZPageAllocatorStats& stats);
    void AtRelocateEnd(const ZPageAllocatorStats& stats, bool recordStats);

    static size_t MaxCapacity();
    size_t UsedAtCollectionStart() const;
    size_t UsedAtMarkStart() const;
    size_t UsedGenerationAtMarkStart() const;
    size_t LiveAtMarkEnd() const;
    size_t AllocatedAtMarkEnd() const;
    size_t GarbageAtMarkEnd() const;
    size_t UsedAtRelocateEnd() const;
    size_t UsedAtCollectionEnd() const;
    // Host pacing/readback (zDriver epilogue, rec=cycle): the reclaimed
    // figure of the finished collection.
    size_t ReclaimedAtRelocateEnd() const;
    size_t StallsAtMarkStart() const;
    size_t StallsAtMarkEnd() const;
    size_t StallsAtRelocateStart() const;
    size_t StallsAtRelocateEnd() const;

    double ReclaimedAvg();
    ZStatHeapStats Stats();

    void Print(const ZGeneration* generation) const;
    void PrintStalls() const;

private:
    mutable std::mutex _statLock;

    struct ZAtInitialize {
        size_t minCapacity = 0;
        size_t maxCapacity = 0;
    };
    static ZAtInitialize _atInitialize;

    struct ZAtCollectionStart {
        size_t softMaxCapacity = 0;
        size_t capacity = 0;
        size_t free = 0;
        size_t used = 0;
        size_t usedGeneration = 0;
    } _atCollectionStart;

    struct ZAtMarkStart {
        size_t softMaxCapacity = 0;
        size_t capacity = 0;
        size_t free = 0;
        size_t used = 0;
        size_t usedGeneration = 0;
        size_t allocationStalls = 0;
    } _atMarkStart;

    struct ZAtMarkEnd {
        size_t capacity = 0;
        size_t free = 0;
        size_t used = 0;
        size_t usedGeneration = 0;
        size_t live = 0;
        size_t garbage = 0;
        size_t mutatorAllocated = 0;
        size_t allocationStalls = 0;
    } _atMarkEnd;

    struct ZAtRelocateStart {
        size_t capacity = 0;
        size_t free = 0;
        size_t used = 0;
        size_t usedGeneration = 0;
        size_t live = 0;
        size_t garbage = 0;
        size_t mutatorAllocated = 0;
        size_t reclaimed = 0;
        size_t promoted = 0;
        size_t compacted = 0;
        size_t allocationStalls = 0;
    } _atRelocateStart;

    struct ZAtRelocateEnd {
        size_t capacity = 0;
        size_t capacityHigh = 0;
        size_t capacityLow = 0;
        size_t free = 0;
        size_t freeHigh = 0;
        size_t freeLow = 0;
        size_t used = 0;
        size_t usedHigh = 0;
        size_t usedLow = 0;
        size_t usedGeneration = 0;
        size_t live = 0;
        size_t garbage = 0;
        size_t mutatorAllocated = 0;
        size_t reclaimed = 0;
        size_t promoted = 0;
        size_t compacted = 0;
        size_t allocationStalls = 0;
    } _atRelocateEnd;

    // utilities/numberSeq.cpp alpha=0.7, same decay as ZStatCycle.
    ZStatNumberSeq _reclaimedBytes;

    size_t CapacityHigh() const;
    size_t CapacityLow() const;
    size_t Free(size_t used) const;
    size_t MutatorAllocated(size_t usedGeneration, size_t freed, size_t relocated) const;
    size_t Garbage(size_t freed, size_t relocated, size_t promoted) const;
    size_t Reclaimed(size_t freed, size_t relocated, size_t promoted) const;
};

class ZWorkers;
class RegionManager;

class ZStat final : public ZThread {
public:
    static uint64_t GetPrevGCStartTime() { return prevGcStartTime.load(std::memory_order_acquire); }
    static void SetPrevGCStartTime(uint64_t timestamp) { prevGcStartTime.store(timestamp, std::memory_order_release); }
    static uint64_t GetPrevGCFinishTime() { return prevGcFinishTime.load(std::memory_order_acquire); }
    static void SetPrevGCFinishTime(uint64_t timestamp) { prevGcFinishTime.store(timestamp, std::memory_order_release); }

    ZStat();
    ~ZStat() override = default;
    void run_thread() override;
    void terminate() override;
    static void Initialize();
    // Host infra difference (heap is constructed at runtime, after the
    // harness may have requested init): the per-CPU storage is allocated
    // once both init was requested and the heap singleton (whose
    // ZStatHeap counters are runtime-constructed) exists.
    static void NotifyHeapConstructed();
    private:
    // zStat.hpp:387-389: the sampling thread ticks off a ZMetronome.
    static constexpr uint64_t SampleHz = 1;
    ZMetronome metronome;
    static std::atomic<uint64_t> prevGcStartTime;
    static std::atomic<uint64_t> prevGcFinishTime;
};

// zStat.hpp:484-487, zStat.cpp:1410-1420: system load average, printed as
// an absolute value and as a share of the CPU count.
class ZStatLoad {
public:
    static void Print();
};

// zStat.hpp:492-511, zStat.cpp:1423-1456
class ZStatMark {
public:
    ZStatMark();

    void AtMarkStart(size_t nstripes);
    void AtMarkEnd(size_t nproactiveflush, size_t nterminateflush, size_t ntrycomplete, size_t ncontinue);

    void Print();

private:
    size_t _nstripes;
    size_t _nproactiveflush;
    size_t _nterminateflush;
    size_t _ntrycomplete;
    size_t _ncontinue;
    size_t _markStackUsage;
};

// zStat.hpp:514-553, zStat.cpp:1460-1620 — per-generation relocation account:
// selector snapshot + forwarding usage + in-place counts, printed as a page
// summary and (young only) an age table from ZStatPhaseGeneration::RegisterEnd.
struct ZStatRelocationSummary {
    size_t npagesCandidates = 0;
    size_t total = 0;
    size_t live = 0;
    size_t empty = 0;
    size_t npagesSelected = 0;
    size_t relocate = 0;
};

class ZStatRelocation {
public:
    ZStatRelocation();

    void AtSelectRelocationSet(const ZRelocationSetSelectorStats& selectorStats);
    void AtInstallRelocationSet(size_t forwardingUsage);
    void AtRelocateEnd(size_t smallInPlaceCount, size_t mediumInPlaceCount);

    void PrintPageSummary();
    void PrintAgeTable();

private:
    ZRelocationSetSelectorStats _selectorStats;
    size_t _forwardingUsage = 0;
    size_t _smallSelected = 0;
    size_t _smallInPlaceCount = 0;
    size_t _mediumSelected = 0;
    size_t _mediumInPlaceCount = 0;
};

// zStat.hpp:568-585, zStat.cpp:1642-1697
class ZStatReferences {public:
    static void set_soft(size_t encountered, size_t discovered, size_t enqueued);
    static void set_weak(size_t encountered, size_t discovered, size_t enqueued);
    static void set_final(size_t encountered, size_t discovered, size_t enqueued);
    static void set_phantom(size_t encountered, size_t discovered, size_t enqueued);

    static void Print();

private:
    struct ZCount {
        size_t encountered = 0;
        size_t discovered = 0;
        size_t enqueued = 0;
    };
    static ZCount soft;
    static ZCount weak;
    static ZCount final;
    static ZCount phantom;

    static void Set(ZCount* count, size_t encountered, size_t discovered, size_t enqueued);
};

extern std::atomic<uint64_t> g_gcTotalTimeUs;
extern std::atomic<size_t> g_gcCollectedTotalBytes;

} // namespace MapleRuntime
#endif // MRT_ZSTAT_H
