// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MinorGCALot.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Collector/Collector.h"
#include "Collector/GcRequest.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_allocCount{ 0 };
std::atomic<size_t> g_triggerCount{ 0 };
std::atomic<bool> g_intervalArmed{ false };

size_t ParseInterval()
{
    // Forcing this to a small interval is how the mark-gap was found: ProbeUnmarkedLive lives
    // inside DoYoungGarbageCollection, and a 900s compile of packages/basic ran no young
    // collection at all -- its single young flip came from the major Preforward, which flips both
    // generations.  ALot is a forensic recipe, never a performance one: with interval=2000 the
    // same 900s budget covered 71 young cycles before the remset fix and 44 after it, so any
    // cost measured under ALot is not a production number.
    const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_MINOR_GC_ALOT */;
    if (v == nullptr || v[0] == '\0') {
        return 0;
    }
    char* end = nullptr;
    unsigned long n = std::strtoul(v, &end, 10);
    if (end == v || n == 0) {
        return 0;
    }
    return static_cast<size_t>(n);
}

void ArmYoungIntervalIfNeeded()
{
    bool expected = false;
    if (!g_intervalArmed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    // Drop the 200ms young throttle so ALot can actually raise minor frequency.
    g_gcRequests[GC_REASON_YOUNG].SetMinInterval(0);
    VLOG(REPORT, "[GCV2][alot] armed MRT_GCV2_MINOR_GC_ALOT interval=%zu (young minIntervelNs=0)",
         MinorGCALot::Interval());
}
} // namespace

size_t MinorGCALot::Interval()
{
    static const size_t n = ParseInterval();
    return n;
}

bool MinorGCALot::Enabled() { return Interval() != 0; }

void MinorGCALot::AfterSuccessfulAlloc(size_t allocBytes)
{
    (void)allocBytes;
    const size_t n = Interval();
    if (n == 0) {
        return;
    }
    if (IsGcThread() || IsRuntimeThread()) {
        return;
    }
    ArmYoungIntervalIfNeeded();

    size_t c = g_allocCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c % n != 0) {
        return;
    }
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }
    size_t t = g_triggerCount.fetch_add(1, std::memory_order_relaxed) + 1;
    VLOG(REPORT, "[GCV2][alot] trigger=%zu afterAllocs=%zu interval=%zu request young", t, c, n);
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, true);
}

size_t MinorGCALot::TriggerCount() { return g_triggerCount.load(std::memory_order_relaxed); }
size_t MinorGCALot::AllocCount() { return g_allocCount.load(std::memory_order_relaxed); }

} // namespace MapleRuntime
