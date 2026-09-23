#include "Heap/z/zDirector.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "Base/SysCall.h"
#include "Base/Log.h"
#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/z_globals.hpp"

namespace MapleRuntime {

constexpr double one_in_1000 = 3.290527;

ZDirector* ZDirector::_director = nullptr;

struct ZWorkerResizeStats {
    bool is_active = false;
    double serial_gc_time_passed = 0.0;
    double parallel_gc_time_passed = 0.0;
    uint32_t nworkers_current = 0;
};

struct ZDirectorHeapStats {
    size_t soft_max_heap_size = 0;
    size_t used = 0;
    uint32_t total_collections = 0;
};

struct ZDirectorGenerationGeneralStats {
    size_t used = 0;
    uint32_t total_collections_at_start = 0;
};

struct ZDirectorGenerationStats {
    ZStatCycleStats cycle;
    ZStatWorkersStats workers{};
    ZWorkerResizeStats resize;
    ZStatHeapStats stat_heap;
    ZDirectorGenerationGeneralStats general;
};

struct ZDirectorStats {
    ZStatMutatorAllocRateStats mutator_alloc_rate;
    ZDirectorHeapStats heap;
    ZDirectorGenerationStats young_stats;
    ZDirectorGenerationStats old_stats;
    double collection_interval_sec = 0.0;
};

ZDirector::ZDirector()
{
    _director = this;
    set_name("ZDirector");
    create_and_start();
}

void ZDirector::evaluate_rules()
{
    if (_director == nullptr) {
        return;
    }
    _director->notify_reevaluate();
}

void ZDirector::notify_reevaluate()
{
    std::lock_guard<std::mutex> lock(monitor);
    reevaluate = true;
    condition.notify_one();
}

bool ZDirector::wait_for_tick()
{
    const uint64_t interval_ms = 1000 / DecisionHz;
    std::unique_lock<std::mutex> lock(monitor);
    if (stopped) {
        return false;
    }
    condition.wait_for(lock, std::chrono::milliseconds(interval_ms),
        [this] { return stopped || reevaluate; });
    return !stopped;
}

static uint32_t young_gc_threads(const ZDirectorStats&)
{
    return ZYoungGCThreads == 0 ? 1 : ZYoungGCThreads;
}

static uint32_t old_gc_threads(const ZDirectorStats&)
{
    return ZOldGCThreads == 0 ? 1 : ZOldGCThreads;
}

static bool rule_minor_timer(const ZDirectorStats& stats)
{
    if (stats.collection_interval_sec <= 0) {
        return false;
    }
    const double time_until_gc = stats.collection_interval_sec - stats.young_stats.cycle.timeSinceLast;
    VLOG(REPORT, "Rule Minor: Timer, Interval: %.3fs, TimeUntilGC: %.3fs\n",
        stats.collection_interval_sec, time_until_gc);
    return time_until_gc <= 0;
}

static double estimated_gc_workers(double serial_gc_time, double parallelizable_gc_time, double time_until_deadline)
{
    const double parallelizable_time_until_deadline = std::max(time_until_deadline - serial_gc_time, 0.001);
    return parallelizable_gc_time / parallelizable_time_until_deadline;
}

static uint32_t discrete_young_gc_workers(double gc_workers, uint32_t cap)
{
    const uint32_t limit = cap == 0 ? 1 : cap;
    if (!std::isfinite(gc_workers) || gc_workers >= static_cast<double>(limit)) {
        return limit;
    }
    const uint32_t want = static_cast<uint32_t>(std::ceil(std::max(gc_workers, 1.0)));
    return std::max(1u, std::min(want, limit));
}

static double select_young_gc_workers(const ZDirectorStats& stats, double serial_gc_time,
    double parallelizable_gc_time, double time_until_oom)
{
    const uint32_t cap = young_gc_threads(stats);
    if (!stats.old_stats.cycle.isWarm) {
        VLOG(REPORT, "Select Minor GC Workers (Not Warm), GCWorkers: %.3f\n", static_cast<double>(cap));
        return static_cast<double>(cap);
    }
    const double gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, time_until_oom);
    const uint32_t actual_gc_workers = discrete_young_gc_workers(gc_workers, cap);
    const double last_gc_workers = stats.young_stats.cycle.lastActiveWorkers;
    if (static_cast<double>(actual_gc_workers) < last_gc_workers) {
        const double gc_duration_delta =
            (parallelizable_gc_time / static_cast<double>(actual_gc_workers)) - (parallelizable_gc_time / last_gc_workers);
        const double additional_time_for_allocations = stats.young_stats.cycle.timeSinceLast - gc_duration_delta;
        const double next_time_until_oom = time_until_oom + additional_time_for_allocations;
        const double next_avoid_oom_gc_workers =
            estimated_gc_workers(serial_gc_time, parallelizable_gc_time, next_time_until_oom);
        const double next_gc_workers = next_avoid_oom_gc_workers + 0.5;
        const double lo = static_cast<double>(actual_gc_workers);
        const double try_lowering_gc_workers = next_gc_workers < lo ? lo :
            (next_gc_workers > last_gc_workers ? last_gc_workers : next_gc_workers);
        VLOG(REPORT, "Select Minor GC Workers (Try Lowering), AvoidOOMGCWorkers: %.3f, "
            "NextAvoidOOMGCWorkers: %.3f, LastGCWorkers: %.3f, GCWorkers: %.3f\n",
            gc_workers, next_avoid_oom_gc_workers, last_gc_workers, try_lowering_gc_workers);
        return try_lowering_gc_workers;
    }
    VLOG(REPORT, "Select Minor GC Workers (Normal), AvoidOOMGCWorkers: %.3f, LastGCWorkers: %.3f, GCWorkers: %.3f\n",
        gc_workers, last_gc_workers, gc_workers);
    return gc_workers;
}

static ZDriverRequest rule_minor_allocation_rate_dynamic(const ZDirectorStats& stats, double serial_gc_time_passed,
    double parallel_gc_time_passed, bool conservative_alloc_rate, size_t capacity)
{
    if (!stats.old_stats.cycle.isTimeTrustable) {
        return ZDriverRequest(GC_REASON_INVALID, young_gc_threads(stats), 0);
    }
    const size_t used = stats.heap.used;
    const size_t free_including_headroom = capacity - std::min(capacity, used);
    const size_t free = free_including_headroom - std::min(free_including_headroom, ZHeuristics::relocation_headroom());
    const auto alloc_rate_stats = stats.mutator_alloc_rate;
    const double alloc_rate_sd_percent = alloc_rate_stats.sd / (alloc_rate_stats.avg + 1.0);
    const double alloc_rate_conservative =
        (std::max(alloc_rate_stats.predict, alloc_rate_stats.avg) * ZAllocationSpikeTolerance) +
        (alloc_rate_stats.sd * one_in_1000) + 1.0;
    const double alloc_rate = conservative_alloc_rate ? alloc_rate_conservative : alloc_rate_stats.avg;
    const double time_until_oom = (static_cast<double>(free) / alloc_rate) / (1.0 + alloc_rate_sd_percent);
    const double serial_gc_time = std::fabs(stats.young_stats.cycle.serialTime +
        (stats.young_stats.cycle.serialTimeSd * one_in_1000) - serial_gc_time_passed);
    const double parallelizable_gc_time = std::fabs(stats.young_stats.cycle.parallelTime +
        (stats.young_stats.cycle.parallelTimeSd * one_in_1000) - parallel_gc_time_passed);
    const double gc_workers =
        select_young_gc_workers(stats, serial_gc_time, parallelizable_gc_time, time_until_oom);
    const uint32_t actual_gc_workers = discrete_young_gc_workers(gc_workers, young_gc_threads(stats));
    const double actual_gc_duration = serial_gc_time + (parallelizable_gc_time / actual_gc_workers);
    const double time_until_gc = time_until_oom - actual_gc_duration;
    VLOG(REPORT, "Rule Minor: Allocation Rate (Dynamic GC Workers), MaxAllocRate: %.1fMB/s (+/-%.1f%%), "
        "Free: %zuMB, GCCPUTime: %.3f, GCDuration: %.3fs, TimeUntilOOM: %.3fs, TimeUntilGC: %.3fs, GCWorkers: %u\n",
        alloc_rate / MB, alloc_rate_sd_percent * 100, free / MB, serial_gc_time + parallelizable_gc_time,
        actual_gc_duration, time_until_oom, time_until_gc, actual_gc_workers);
    if (time_until_gc > time_until_oom * 0.05) {
        return ZDriverRequest(GC_REASON_INVALID, actual_gc_workers, 0);
    }
    return ZDriverRequest(GC_REASON_HEU, actual_gc_workers, 0);
}

static ZDriverRequest rule_soft_minor_allocation_rate_dynamic(const ZDirectorStats& stats,
    double serial_gc_time_passed, double parallel_gc_time_passed)
{
    return rule_minor_allocation_rate_dynamic(stats, 0.0 /* serial_gc_time_passed */,
        0.0 /* parallel_gc_time_passed */, false,
        stats.heap.soft_max_heap_size);
}

static ZDriverRequest rule_semi_hard_minor_allocation_rate_dynamic(const ZDirectorStats& stats,
    double serial_gc_time_passed, double parallel_gc_time_passed)
{
    return rule_minor_allocation_rate_dynamic(stats, 0.0 /* serial_gc_time_passed */,
        0.0 /* parallel_gc_time_passed */, false,
        Heap::GetHeap().GetMaxCapacity());
}

static ZDriverRequest rule_hard_minor_allocation_rate_dynamic(const ZDirectorStats& stats,
    double serial_gc_time_passed, double parallel_gc_time_passed)
{
    return rule_minor_allocation_rate_dynamic(stats, 0.0 /* serial_gc_time_passed */,
        0.0 /* parallel_gc_time_passed */, true,
        Heap::GetHeap().GetMaxCapacity());
}

static bool rule_minor_allocation_rate_static(const ZDirectorStats& stats)
{
    if (!stats.old_stats.cycle.isTimeTrustable) {
        return false;
    }
    const size_t soft_max_capacity = stats.heap.soft_max_heap_size;
    const size_t used = stats.heap.used;
    const size_t free_including_headroom = soft_max_capacity - std::min(soft_max_capacity, used);
    const size_t free = free_including_headroom - std::min(free_including_headroom, ZHeuristics::relocation_headroom());
    const auto alloc_rate_stats = stats.mutator_alloc_rate;
    const double max_alloc_rate =
        (alloc_rate_stats.avg * ZAllocationSpikeTolerance) + (alloc_rate_stats.sd * one_in_1000);
    const double time_until_oom = static_cast<double>(free) / (max_alloc_rate + 1.0);
    const double serial_gc_time =
        stats.young_stats.cycle.serialTime + (stats.young_stats.cycle.serialTimeSd * one_in_1000);
    const double parallelizable_gc_time =
        stats.young_stats.cycle.parallelTime + (stats.young_stats.cycle.parallelTimeSd * one_in_1000);
    const double gc_duration = serial_gc_time + (parallelizable_gc_time / young_gc_threads(stats));
    const double time_until_gc = time_until_oom - gc_duration;
    VLOG(REPORT, "Rule Minor: Allocation Rate (Static GC Workers), MaxAllocRate: %.1fMB/s, "
        "Free: %zuMB, GCDuration: %.3fs, TimeUntilGC: %.3fs\n",
        max_alloc_rate / MB, free / MB, gc_duration, time_until_gc);
    return time_until_gc <= 0;
}

static bool is_young_small(const ZDirectorStats& stats)
{
    const size_t soft_max_capacity = stats.heap.soft_max_heap_size;
    if (soft_max_capacity == 0) {
        return true;
    }
    const double young_used_percent =
        100.0 * static_cast<double>(stats.young_stats.general.used) / static_cast<double>(soft_max_capacity);
    return young_used_percent <= 5.0;
}

static bool is_high_usage(const ZDirectorStats& stats, bool log = false)
{
    const size_t soft_max_capacity = stats.heap.soft_max_heap_size;
    if (soft_max_capacity == 0) {
        return true;
    }
    const size_t used = stats.heap.used;
    const size_t free_including_headroom = soft_max_capacity - std::min(soft_max_capacity, used);
    const size_t free = free_including_headroom - std::min(free_including_headroom, ZHeuristics::relocation_headroom());
    const double free_percent = 100.0 * static_cast<double>(free) / static_cast<double>(soft_max_capacity);
    if (log) {
        VLOG(REPORT, "Rule Minor: High Usage, Free: %zuMB(%.1f%%)\n", free / MB, free_percent);
    }
    return free_percent <= 5.0;
}

static bool is_major_urgent(const ZDirectorStats& stats)
{
    return is_young_small(stats) && is_high_usage(stats);
}

static bool rule_minor_allocation_rate(const ZDirectorStats& stats)
{
    if (ZCollectionIntervalOnly) {
        return false;
    }
    const bool stalling_for_old = Heap::GetHeap().page_allocator().IsAllocationStallingForOld();
    if (stalling_for_old) {
        VLOG(REPORT, "Rule Minor: Allocation Stall, StallingForOld: %d, Stalling: %d, Suppressed: 1\n",
            stalling_for_old, Heap::GetHeap().page_allocator().IsAllocationStalling());
        return false;
    }
    VLOG(REPORT, "Rule Minor: Allocation Stall, StallingForOld: %d, Stalling: %d, Suppressed: 0\n",
        stalling_for_old, Heap::GetHeap().page_allocator().IsAllocationStalling());
    if (is_young_small(stats)) {
        return false;
    }
    if (UseDynamicNumberOfGCThreads) {
        if (rule_soft_minor_allocation_rate_dynamic(stats, 0.0, 0.0).cause() != GC_REASON_INVALID) {
            return true;
        }
        if (rule_hard_minor_allocation_rate_dynamic(stats, 0.0, 0.0).cause() != GC_REASON_INVALID) {
            return true;
        }
        return false;
    }
    return rule_minor_allocation_rate_static(stats);
}

static bool rule_minor_high_usage(const ZDirectorStats& stats)
{
    if (ZCollectionIntervalOnly) {
        return false;
    }
    if (is_young_small(stats)) {
        return false;
    }
    return is_high_usage(stats, true);
}

static bool rule_major_timer(const ZDirectorStats& stats)
{
    if (stats.collection_interval_sec <= 0) {
        return false;
    }
    const double time_until_gc = stats.collection_interval_sec - stats.old_stats.cycle.timeSinceLast;
    VLOG(REPORT, "Rule Major: Timer, Interval: %.3fs, TimeUntilGC: %.3fs\n",
        stats.collection_interval_sec, time_until_gc);
    return time_until_gc <= 0;
}

static bool rule_major_warmup(const ZDirectorStats& stats)
{
    if (ZCollectionIntervalOnly) {
        return false;
    }
    if (stats.old_stats.cycle.isWarm) {
        return false;
    }
    const size_t soft_max_capacity = stats.heap.soft_max_heap_size;
    const double used_threshold_percent = (stats.old_stats.cycle.warmupCycles + 1) * 0.1;
    const size_t used_threshold = static_cast<size_t>(soft_max_capacity * used_threshold_percent);
    VLOG(REPORT, "Rule Major: Warmup %.0f%%, Used: %zuMB, UsedThreshold: %zuMB\n",
        used_threshold_percent * 100, stats.heap.used / MB, used_threshold / MB);
    return stats.heap.used >= used_threshold;
}

static double gc_time(const ZDirectorGenerationStats& generation_stats)
{
    const double serial_gc_time =
        generation_stats.cycle.serialTime + (generation_stats.cycle.serialTimeSd * one_in_1000);
    const double parallelizable_gc_time =
        generation_stats.cycle.parallelTime + (generation_stats.cycle.parallelTimeSd * one_in_1000);
    return serial_gc_time + parallelizable_gc_time;
}

static double calculate_extra_young_gc_time(const ZDirectorStats& stats)
{
    if (!stats.old_stats.cycle.isTimeTrustable) {
        return 0.0;
    }
    const size_t old_used = stats.old_stats.general.used;
    const size_t old_live = stats.old_stats.stat_heap.liveAtMarkEnd;
    const double old_garbage = static_cast<double>(old_used - old_live);
    const double young_gc_time = gc_time(stats.young_stats);
    const double reclaimed_per_young_gc = stats.young_stats.stat_heap.reclaimedAverage;
    const double current_young_gc_time_per_bytes_freed = young_gc_time / reclaimed_per_young_gc;
    const double potential_young_gc_time_per_bytes_freed = young_gc_time / (reclaimed_per_young_gc + old_garbage);
    if (current_young_gc_time_per_bytes_freed == std::numeric_limits<double>::infinity()) {
        return std::numeric_limits<double>::infinity();
    }
    const double extra_young_gc_time_per_bytes_freed =
        current_young_gc_time_per_bytes_freed - potential_young_gc_time_per_bytes_freed;
    return extra_young_gc_time_per_bytes_freed * (reclaimed_per_young_gc + old_garbage);
}

static bool rule_major_allocation_rate(const ZDirectorStats& stats)
{
    if (!stats.old_stats.cycle.isTimeTrustable) {
        return false;
    }
    const double old_gc_time = gc_time(stats.old_stats);
    const double young_gc_time = gc_time(stats.young_stats);
    const double reclaimed_per_young_gc = stats.young_stats.stat_heap.reclaimedAverage;
    const double reclaimed_per_old_gc = stats.old_stats.stat_heap.reclaimedAverage;
    const double current_young_gc_time_per_bytes_freed = young_gc_time / reclaimed_per_young_gc;
    const double current_old_gc_time_per_bytes_freed = old_gc_time / reclaimed_per_old_gc;
    const double extra_young_gc_time = calculate_extra_young_gc_time(stats);
    const uint32_t lookahead = stats.heap.total_collections - stats.old_stats.general.total_collections_at_start;
    const double extra_young_gc_time_for_lookahead = extra_young_gc_time * static_cast<double>(lookahead);
    VLOG(REPORT, "Rule Major: Allocation Rate, ExtraYoungGCTime: %.3fs, OldGCTime: %.3fs, "
        "Lookahead: %u, ExtraYoungGCTimeForLookahead: %.3fs\n",
        extra_young_gc_time, old_gc_time, lookahead, extra_young_gc_time_for_lookahead);
    const bool can_amortize_time_cost = extra_young_gc_time_for_lookahead > old_gc_time;
    const bool old_garbage_is_cheaper = current_old_gc_time_per_bytes_freed < current_young_gc_time_per_bytes_freed;
    return can_amortize_time_cost || old_garbage_is_cheaper || is_major_urgent(stats);
}

static double calculate_young_to_old_worker_ratio(const ZDirectorStats& stats)
{
    if (!stats.old_stats.cycle.isTimeTrustable) {
        return 1.0;
    }
    const double young_gc_time = gc_time(stats.young_stats);
    const double old_gc_time = gc_time(stats.old_stats);
    const double reclaimed_per_young_gc = stats.young_stats.stat_heap.reclaimedAverage;
    const double reclaimed_per_old_gc = stats.old_stats.stat_heap.reclaimedAverage;
    const double current_young_bytes_freed_per_gc_time = reclaimed_per_young_gc / young_gc_time;
    const double current_old_bytes_freed_per_gc_time = reclaimed_per_old_gc / old_gc_time;
    if (current_young_bytes_freed_per_gc_time == 0.0) {
        return current_old_bytes_freed_per_gc_time == 0.0 ? 1.0 : static_cast<double>(old_gc_threads(stats));
    }
    return std::min(current_old_bytes_freed_per_gc_time / current_young_bytes_freed_per_gc_time,
        static_cast<double>(old_gc_threads(stats)));
}

static bool rule_major_proactive(const ZDirectorStats& stats)
{
    if (ZCollectionIntervalOnly) {
        return false;
    }
    if (!ZProactive) {
        return false;
    }
    if (!stats.old_stats.cycle.isWarm) {
        return false;
    }
    const size_t used_after_last_gc = stats.old_stats.stat_heap.usedAtRelocateEnd;
    const size_t used_increase_threshold = static_cast<size_t>(stats.heap.soft_max_heap_size * 0.10);
    const size_t used_threshold = used_after_last_gc + used_increase_threshold;
    if (stats.heap.used < used_threshold && stats.old_stats.cycle.timeSinceLast < 5 * 60) {
        VLOG(REPORT, "Rule Major: Proactive, UsedUntilEnabled: %zuMB, TimeUntilEnabled: %.3fs\n",
            (used_threshold - stats.heap.used) / MB, 5 * 60 - stats.old_stats.cycle.timeSinceLast);
        return false;
    }
    const double serial_gc_time = gc_time(stats.old_stats) + gc_time(stats.young_stats);
    const double acceptable_gc_interval = serial_gc_time * ((0.50 / 0.01) - 1.0);
    const double time_until_gc = acceptable_gc_interval - stats.old_stats.cycle.timeSinceLast;
    VLOG(REPORT, "Rule Major: Proactive, AcceptableGCInterval: %.3fs, TimeSinceLastGC: %.3fs, TimeUntilGC: %.3fs\n",
        acceptable_gc_interval, stats.old_stats.cycle.timeSinceLast, time_until_gc);
    return time_until_gc <= 0;
}

static GCReason make_minor_gc_decision(const ZDirectorStats& stats)
{
    if (ZCollectedHeap::heap()->driver_minor()->port().is_busy()) {
        return GC_REASON_INVALID;
    }
    if (ZCollectedHeap::heap()->driver_major()->port().is_busy() && !stats.old_stats.resize.is_active) {
        return GC_REASON_INVALID;
    }
    if (rule_minor_timer(stats)) {
        return GC_REASON_BACKUP;
    }
    if (rule_minor_allocation_rate(stats)) {
        return GC_REASON_HEU;
    }
    if (rule_minor_high_usage(stats)) {
        return GC_REASON_HEU;
    }
    return GC_REASON_INVALID;
}

static GCReason make_major_gc_decision(const ZDirectorStats& stats)
{
    if (ZCollectedHeap::heap()->driver_major()->port().is_busy()) {
        return GC_REASON_INVALID;
    }
    if (rule_major_timer(stats)) {
        return GC_REASON_BACKUP;
    }
    if (rule_major_warmup(stats)) {
        return GC_REASON_WARMUP;
    }
    if (rule_major_proactive(stats)) {
        return GC_REASON_HEU;
    }
    return GC_REASON_INVALID;
}

enum class ZWorkerSelectionType {
    start_major,
    minor_during_old,
    normal
};

struct ZWorkerCounts {
    uint32_t young_workers;
    uint32_t old_workers;
};

static uint32_t clamp_workers(uint32_t count, uint32_t hi)
{
    const uint32_t cap = hi == 0 ? 1 : hi;
    if (count < 1) {
        return 1;
    }
    return count > cap ? cap : count;
}

static ZWorkerCounts select_worker_threads(const ZDirectorStats& stats, uint32_t young_workers,
    ZWorkerSelectionType type)
{
    const uint32_t active_young_workers = stats.young_stats.resize.nworkers_current;
    const uint32_t active_old_workers = stats.old_stats.resize.nworkers_current;
    if (Heap::GetHeap().page_allocator().IsAllocationStalling()) {
        return {ZYoungGCThreads, ZOldGCThreads};
    }
    if (active_young_workers + active_old_workers > ConcGCThreads) {
        return {active_young_workers, active_old_workers};
    }
    const double young_to_old_ratio = calculate_young_to_old_worker_ratio(stats);
    uint32_t old_workers = clamp_workers(static_cast<uint32_t>(young_workers * young_to_old_ratio), ZOldGCThreads);
    if (type != ZWorkerSelectionType::normal && old_workers + young_workers > ConcGCThreads) {
        const double old_ratio = young_to_old_ratio / (1.0 + young_to_old_ratio);
        const double young_ratio = 1.0 - old_ratio;
        const uint32_t young_workers_clamped =
            clamp_workers(static_cast<uint32_t>(ConcGCThreads * young_ratio), ZYoungGCThreads);
        const uint32_t old_workers_clamped =
            clamp_workers(ConcGCThreads - young_workers_clamped, ZOldGCThreads);
        if (type == ZWorkerSelectionType::start_major) {
            old_workers = old_workers_clamped;
            young_workers = clamp_workers(std::max(old_workers, young_workers), ZYoungGCThreads);
        } else if (type == ZWorkerSelectionType::minor_during_old) {
            young_workers = young_workers_clamped;
            old_workers = old_workers_clamped;
        }
    }
    return {young_workers, old_workers};
}

static void adjust_gc(const ZDirectorStats& stats)
{
    if (!UseDynamicNumberOfGCThreads) {
        return;
    }
    if (!stats.young_stats.resize.is_active) {
        return;
    }
    const ZDriverRequest request = rule_semi_hard_minor_allocation_rate_dynamic(stats,
        stats.young_stats.resize.serial_gc_time_passed, stats.young_stats.resize.parallel_gc_time_passed);
    if (request.cause() == GC_REASON_INVALID) {
        return;
    }
    uint32_t desired_young_workers = std::max(request.young_nworkers(), stats.young_stats.resize.nworkers_current);
    if (desired_young_workers > stats.young_stats.resize.nworkers_current) {
        const uint32_t needed_young_increase = desired_young_workers - stats.young_stats.resize.nworkers_current;
        desired_young_workers = std::min(stats.young_stats.resize.nworkers_current + needed_young_increase * 2,
            ZYoungGCThreads);
    }
    const ZWorkerSelectionType type = stats.old_stats.resize.is_active ? ZWorkerSelectionType::minor_during_old :
                                                                       ZWorkerSelectionType::normal;
    const ZWorkerCounts selection = select_worker_threads(stats, desired_young_workers, type);
    if (stats.old_stats.resize.is_active &&
        stats.old_stats.resize.nworkers_current != selection.old_workers) {
        Heap::GetHeap().old().Workers()->request_resize_workers(selection.old_workers);
    }
    if (stats.young_stats.resize.nworkers_current != selection.young_workers) {
        Heap::GetHeap().young().Workers()->request_resize_workers(selection.young_workers);
    }
}

static ZWorkerCounts initial_workers(const ZDirectorStats& stats, ZWorkerSelectionType type)
{
    if (!UseDynamicNumberOfGCThreads) {
        return {ZYoungGCThreads, ZOldGCThreads};
    }
    const ZDriverRequest soft_request = rule_soft_minor_allocation_rate_dynamic(stats, 0.0, 0.0);
    const ZDriverRequest hard_request = rule_hard_minor_allocation_rate_dynamic(stats, 0.0, 0.0);
    const uint32_t young_workers =
        std::max(1u, std::max(soft_request.young_nworkers(), hard_request.young_nworkers()));
    return select_worker_threads(stats, young_workers, type);
}

static void start_major_gc(const ZDirectorStats& stats, GCReason cause)
{
    const ZWorkerCounts selection = initial_workers(stats, ZWorkerSelectionType::start_major);
    ZCollectedHeap::heap()->driver_major()->port().send_async(
        ZDriverRequest(cause, selection.young_workers, selection.old_workers));
}

static void start_minor_gc(const ZDirectorStats& stats, GCReason cause)
{
    const ZWorkerSelectionType type =
        ZCollectedHeap::heap()->driver_major()->port().is_busy() ? ZWorkerSelectionType::minor_during_old :
                                                                ZWorkerSelectionType::normal;
    const ZWorkerCounts selection = initial_workers(stats, type);
    if (UseDynamicNumberOfGCThreads && ZCollectedHeap::heap()->driver_major()->port().is_busy()) {
        const ZWorkerResizeStats old_resize_stats = stats.old_stats.resize;
        const uint32_t old_current_workers = old_resize_stats.nworkers_current;

        if (old_current_workers != selection.old_workers) {
            Heap::GetHeap().old().Workers()->request_resize_workers(selection.old_workers);
        }
    }
    ZCollectedHeap::heap()->driver_minor()->port().send_async(ZDriverRequest(cause, selection.young_workers, 0));
}

static bool start_gc(const ZDirectorStats& stats)
{
    const GCReason major_cause = make_major_gc_decision(stats);
    if (major_cause != GC_REASON_INVALID) {
        start_major_gc(stats, major_cause);
        return true;
    }
    const GCReason minor_cause = make_minor_gc_decision(stats);
    if (minor_cause != GC_REASON_INVALID) {
        if (!ZCollectedHeap::heap()->driver_major()->port().is_busy() && rule_major_allocation_rate(stats)) {
            start_major_gc(stats, GC_REASON_HEU);
        } else {
            start_minor_gc(stats, minor_cause);
        }
        return true;
    }
    return false;
}

static ZWorkerResizeStats sample_worker_resize_stats(const ZStatCycleStats& cycle_stats,
    ZStatWorkersStats worker_stats, ZWorkers* workers)
{
    std::lock_guard<std::mutex> locker(*workers->resizing_lock());
    if (!workers->is_active()) {
        return {};
    }
    const double parallel_gc_duration_passed = worker_stats._accumulated_duration;
    const double parallel_gc_time_passed = worker_stats._accumulated_time;
    const double serial_gc_time_passed = cycle_stats.durationSinceStart - parallel_gc_duration_passed;
    return {true, serial_gc_time_passed, parallel_gc_time_passed, workers->active_workers()};
}

static ZDirectorStats sample_stats()
{
    const uint64_t now = TimeUtil::NanoSeconds();
    auto& regions = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    ZDirectorStats stats;
    stats.mutator_alloc_rate = ZStatMutatorAllocRate::stats();
    stats.heap.soft_max_heap_size = Heap::GetHeap().soft_max_capacity();
    stats.heap.used = Heap::GetHeap().GetAllocator().AllocatedBytes();
    stats.heap.total_collections = Heap::GetHeap().total_collections();
    stats.young_stats.cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::young).CycleStats().Stats(now);
    stats.old_stats.cycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old).CycleStats().Stats(now);
    stats.young_stats.workers = Heap::GetHeap().GetZGeneration(ZGenerationId::young).StatWorkers()->stats();
    stats.old_stats.workers = Heap::GetHeap().GetZGeneration(ZGenerationId::old).StatWorkers()->stats();
    stats.young_stats.resize = sample_worker_resize_stats(stats.young_stats.cycle, stats.young_stats.workers,
        Heap::GetHeap().young().Workers());
    stats.old_stats.resize = sample_worker_resize_stats(stats.old_stats.cycle, stats.old_stats.workers,
        Heap::GetHeap().old().Workers());
    stats.young_stats.stat_heap = Heap::GetHeap().GetZGeneration(ZGenerationId::young).StatHeap()->Stats();
    stats.old_stats.stat_heap = Heap::GetHeap().GetZGeneration(ZGenerationId::old).StatHeap()->Stats();
    stats.young_stats.general.used = regions.used_generation(ZGenerationId::young);
    stats.old_stats.general.used = regions.used_generation(ZGenerationId::old);
    stats.old_stats.general.total_collections_at_start = Heap::GetHeap().old().total_collections_at_start();
    stats.collection_interval_sec =
        static_cast<double>(CangjieRuntime::GetGCParam().backupGCInterval) / SECOND_TO_NANO_SECOND;
    return stats;
}

void ZDirector::run_thread()
{
    while (wait_for_tick()) {
        reevaluate = false;
        if (Runtime::CurrentRef() == nullptr || !Heap::GetHeap().IsGCEnabled()) {
            continue;
        }
        const ZDirectorStats stats = sample_stats();
        if (!MapleRuntime::start_gc(stats)) {
            adjust_gc(stats);
        }
    }
}

void ZDirector::terminate()
{
    std::lock_guard<std::mutex> locker(monitor);
    stopped = true;
    condition.notify_all();
}

} // namespace MapleRuntime
