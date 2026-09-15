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

#include "Heap/z/zThread.hpp"

namespace MapleRuntime {
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
    // utilities/numberSeq.cpp:35-49,79-94: exponentially decaying mean
    // and variance, alpha=0.7 as used by ZStatCycle.
    struct Sequence {
        void Add(double value);
        bool initialized = false;
        double average = 0;
        double variance = 0;
    };
    mutable std::mutex lock;
    uint64_t start = 0;
    uint64_t end = 0;
    uint32_t warmupCycles = 0;
    double lastActiveWorkers = 1;
    Sequence serial;
    Sequence parallel;
};

// zGeneration.cpp:600-602,637,1248: total collections count young
// mark starts, including the young part of a major. Old completion is
// not another collection start. Keep the total and old baseline together
// so a director sample cannot combine opposite sides of a major start.
struct ZStatCollectionStats {
    uint32_t totalCollections = 0;
    uint32_t collectionsAtMajorStart = 0;
};

class ZStatCollection {
public:
    void AtYoungMarkStart(bool startsOld);
    ZStatCollectionStats Stats() const;

private:
    mutable std::mutex lock;
    ZStatCollectionStats counts;
};


struct ZStatSamplerData;

enum class ZStatUnit { TIME, BYTES, THREADS, BYTES_PER_SECOND, OPS_PER_SECOND };

// Identity and list membership are fixed by static construction, before startup.
class ZStatValue {
public:
    const char* Group() const;
    const char* Name() const;
    uint32_t Id() const;
    ZStatValue(const ZStatValue&) = delete;
    ZStatValue& operator=(const ZStatValue&) = delete;
protected:
    ZStatValue(const char* group, const char* name, uint32_t id, size_t size);
    template<typename T> T* CpuLocal(size_t cpu) const
    {
        return reinterpret_cast<T*>(base + stride * cpu + offset);
    }
    static void InitializeStorage();
    friend class ZStat;
    static size_t CpuCount();
    static size_t CpuId();
private:
    const char* const group;
    const char* const name;
    const uint32_t id;
    const size_t offset;
    static size_t stride;
    static char* base;
};

class ZStatSampler : public ZStatValue {
public:
    ZStatSampler(const char* group, const char* name, ZStatUnit unit);
    void Initialize() const;
    void Sample(uint64_t value) const;
    ZStatSamplerData CollectAndReset() const;
    ZStatUnit Unit() const { return unit; }
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
    const ZStatUnit unit;
};

class ZStatCounter : public ZStatValue {
public:
    ZStatCounter(const char* group, const char* name, ZStatUnit unit);
    void Initialize() const;
    void Increment(uint64_t value = 1) const;
    void SampleAndReset() const;
    static ZStatCounter* First() { return first; }
    const ZStatCounter* Next() const { return next; }
    const ZStatSampler& Sampler() const { return sampler; }
private:
    struct alignas(64) CpuData { std::atomic<uint64_t> value {0}; };
    static ZStatCounter* first;
    static uint32_t count;
    ZStatCounter* const next;
    const ZStatSampler sampler;
};

// zStat.cpp:600-875: phase group and generation are properties of the
// static phase object; neither the observed name nor a cycle table owns it.
class ZStatPhase {
public:
    ZStatPhase(const char* group, const char* name);
    const char* Name() const;
    virtual void RegisterEnd(uint64_t duration) const;
    virtual ~ZStatPhase() = default;
    const ZStatSampler& Sampler() const { return sampler; }
private:
    const ZStatSampler sampler;
};

// zStat.cpp:848-875: critical phases register both duration and frequency.
class ZStatCriticalPhase : public ZStatPhase {
public:
    explicit ZStatCriticalPhase(const char* name);
    void RegisterEnd(uint64_t duration) const override;
private:
    const ZStatCounter counter;
};

namespace ZStatPhases {
extern const ZStatPhase PCollectFromSpaceGarbage;
extern const ZStatPhase PCollectLargeGarbage;
extern const ZStatPhase PConcurrentMarking;
extern const ZStatPhase PConcurrentReMarking;
extern const ZStatPhase PConcurrentResurrection;
extern const ZStatPhase PDoTracing;
extern const ZStatPhase PEnumRootsUpdateOldPointersWithin;
extern const ZStatPhase PExemptFromRegions;
extern const ZStatCriticalPhase PFinalizer;
extern const ZStatCriticalPhase PFinalizerProcessorWaittingTime;
extern const ZStatPhase YoungForwardFromRegions;
extern const ZStatPhase OldForwardFromRegions;
extern const ZStatPhase PIdentifyUselessExternRef;
extern const ZStatPhase POldRelocateStart;
extern const ZStatPhase PPostTrace;
extern const ZStatPhase PPreforward;
extern const ZStatCriticalPhase PReclaimGarbageRegions;
extern const ZStatPhase PRemapYoungRoots;
extern const ZStatPhase PTraceLiveObjectsUpdateOldPointersInRefFields;
extern const ZStatPhase PYoungConcPromoteWalk;
extern const ZStatPhase PYoungConcurrentRelocate;
extern const ZStatPhase PYoungEvacFinish;
extern const ZStatPhase PYoungEvacRetire;
extern const ZStatPhase PYoungFlushAlloc;
extern const ZStatPhase PYoungMarkClosure;
extern const ZStatPhase PYoungMarkFollow;
extern const ZStatPhase PYoungMarkFromRemset;
extern const ZStatPhase PYoungPinnedScan;
extern const ZStatPhase PYoungPostEvacFinish;
extern const ZStatPhase PYoungPreEvacClear;
extern const ZStatPhase PYoungPrepareCandidates;
extern const ZStatPhase PYoungRefFix;
extern const ZStatPhase PYoungRefFixBulk;
extern const ZStatPhase PYoungRefFixPrepare;
extern const ZStatPhase PYoungRefFixRootPass1;
extern const ZStatPhase PYoungRemsetDrain;
extern const ZStatPhase PYoungRemsetRescan;
extern const ZStatPhase PYoungRootEnum;
extern const ZStatPhase YoungGeneration;
extern const ZStatPhase OldGeneration;
extern const ZStatPhase MinorCollection;
extern const ZStatPhase MajorCollection;
}

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
    // zDirector.cpp:867 / zHeap.cpp:61 — SoftMaxHeapSize. Trigger denominator
    // only; allocation failure still uses hard capacity.
    static size_t soft_max_heap_size();

private:
    static void update_sampling_granule();
};


// zStatHeap::stats / at_relocate_end: one synchronized heap account per generation.
struct ZStatHeapStats {
    size_t usedAtRelocateEnd = 0;
    size_t liveAtMarkEnd = 0;
    double reclaimedAverage = 0;
};
class ZStatHeap {
public:
    explicit ZStatHeap(const char* group);
    void AtRelocateEnd(size_t used, size_t live, size_t reclaimedBytes);
    ZStatHeapStats Stats() const;
private:
    const ZStatSampler reclaimed;
    mutable std::mutex lock;
    ZStatHeapStats stats;
    bool initialized = false;
};

struct GcTriggerInputs;
class ZWorkers;
class RegionManager;

// zStat.hpp:385-401, zStat.cpp:1022-1095: the stat thread is a ZThread that
// samples on a metronome tick and prints on the statistics interval. The
// constructor starts the thread; ConcurrentGCThread::stop ends it.
class ZStat final : public ZThread {
public:
    ZStat();
    ~ZStat() override = default;
    void run_thread() override;
    void terminate() override;
    static void Initialize();
    static ZStatCollection& Collections();
    static ZStatHeap& YoungHeap();
    static ZStatHeap& OldHeap();
    // zDirector.cpp:651-678 sample_worker_resize_stats: worker activity is read
    // under ZWorkers::resizing_lock; the worker budget is the ZYoungGCThreads
    // flag in ZGC and the caller's concurrent budget here.
    static GcTriggerInputs SampleDirectorStats(uint64_t now, ZStatCycle& young, ZStatCycle& old,
                                              RegionManager& regions, ZWorkers& youngWorkers, ZWorkers& oldWorkers,
                                              uint32_t workerCapacity);
    // Existing GCLOG kind observer. It does not select sampler identity or group.
    static void EnterStwScope();
    static void ExitStwScope();
    static bool WorldStoppedNow();
private:
    std::mutex lock;
    std::condition_variable condition;
    bool stopped = false;
    static std::atomic<int> stwDepth;
};

} // namespace MapleRuntime
#endif // MRT_ZSTAT_H
