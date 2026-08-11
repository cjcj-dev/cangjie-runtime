// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/O2ORemsetDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace O2ORemsetDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        if (EnvIsOne("MRT_GCV2_O2OREMSET")) {
            return true;
        }
        return DiagGate::TokenOn("o2oremset");
    }();
    return on;
}

std::atomic<uint64_t> g_oldObjFwd{ 0 };
std::atomic<uint64_t> g_oldObjFwdBytes{ 0 };
std::atomic<uint64_t> g_youngObjFwd{ 0 };
std::atomic<uint64_t> g_oldRegionFwd{ 0 };
std::atomic<uint64_t> g_remsetInFromSum{ 0 };
std::atomic<uint64_t> g_remsetInFromNzRegions{ 0 };
std::atomic<uint64_t> g_o2yOnToSum{ 0 };
std::atomic<uint64_t> g_scrubNonYoungCalls{ 0 };
std::atomic<uint64_t> g_scrubNonYoungErased{ 0 };
std::atomic<uint64_t> g_scrubNonYoungNz{ 0 };
std::atomic<size_t> g_sampleLeft{ 16 };

void MaybeSample(const char* tag, RegionInfo* region, size_t a, size_t b, size_t c)
{
    size_t left = g_sampleLeft.load(std::memory_order_relaxed);
    if (left == 0) {
        return;
    }
    if (!g_sampleLeft.compare_exchange_strong(left, left - 1, std::memory_order_relaxed)) {
        return;
    }
    // RTLOG_ERROR: always visible (VLOG(REPORT) is file-gated; same shape as f3why2 health).
    LOG(RTLOG_ERROR,
        "[GCV2][o2oremset][sample] tag=%s region=%p start=%#zx end=%#zx type=%u young=%u "
        "a=%zu b=%zu c=%zu env=MRT_GCV2_O2OREMSET=1",
        tag, region, region != nullptr ? region->GetRegionStart() : 0,
        region != nullptr ? region->GetRegionEnd() : 0,
        region != nullptr ? static_cast<unsigned>(region->GetRegionType()) : 0u,
        region != nullptr ? static_cast<unsigned>(region->IsYoungRegion()) : 0u, a, b, c);
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteOldObjectForward(BaseObject* fromObj, BaseObject* toObj, size_t size)
{
    if (!GateOn()) {
        return;
    }
    (void)fromObj;
    (void)toObj;
    g_oldObjFwd.fetch_add(1, std::memory_order_relaxed);
    g_oldObjFwdBytes.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
}

void NoteYoungObjectForward()
{
    if (!GateOn()) {
        return;
    }
    g_youngObjFwd.fetch_add(1, std::memory_order_relaxed);
}

void NoteOldRegionForwarded(RegionInfo* region, size_t remsetInFrom, size_t liveObjectsForwarded,
                            size_t o2yEdgesOnToObj)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    g_oldRegionFwd.fetch_add(1, std::memory_order_relaxed);
    g_remsetInFromSum.fetch_add(static_cast<uint64_t>(remsetInFrom), std::memory_order_relaxed);
    if (remsetInFrom != 0) {
        g_remsetInFromNzRegions.fetch_add(1, std::memory_order_relaxed);
    }
    g_o2yOnToSum.fetch_add(static_cast<uint64_t>(o2yEdgesOnToObj), std::memory_order_relaxed);
    MaybeSample("old-region-fwd", region, remsetInFrom, liveObjectsForwarded, o2yEdgesOnToObj);
}

void NoteScrubNonYoung(RegionInfo* region, size_t scrubbed)
{
    if (!GateOn()) {
        return;
    }
    g_scrubNonYoungCalls.fetch_add(1, std::memory_order_relaxed);
    g_scrubNonYoungErased.fetch_add(static_cast<uint64_t>(scrubbed), std::memory_order_relaxed);
    if (scrubbed != 0) {
        g_scrubNonYoungNz.fetch_add(1, std::memory_order_relaxed);
        MaybeSample("scrub-non-young", region, scrubbed, 0, 0);
    }
}

void DumpAndMaybeReset(const char* point, bool reset)
{
    if (!GateOn()) {
        return;
    }
    uint64_t obj = g_oldObjFwd.load(std::memory_order_relaxed);
    uint64_t bytes = g_oldObjFwdBytes.load(std::memory_order_relaxed);
    uint64_t young = g_youngObjFwd.load(std::memory_order_relaxed);
    uint64_t reg = g_oldRegionFwd.load(std::memory_order_relaxed);
    uint64_t remSum = g_remsetInFromSum.load(std::memory_order_relaxed);
    uint64_t remNz = g_remsetInFromNzRegions.load(std::memory_order_relaxed);
    uint64_t o2y = g_o2yOnToSum.load(std::memory_order_relaxed);
    uint64_t scrubCalls = g_scrubNonYoungCalls.load(std::memory_order_relaxed);
    uint64_t scrubErased = g_scrubNonYoungErased.load(std::memory_order_relaxed);
    uint64_t scrubNz = g_scrubNonYoungNz.load(std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[GCV2][o2oremset] point=%s oldObjFwd=%llu oldObjFwdBytes=%llu youngObjFwd=%llu "
        "oldRegionFwd=%llu remsetInFromSum=%llu remsetInFromNzRegions=%llu "
        "o2yEdgesOnToObjSum=%llu scrubNonYoungCalls=%llu scrubNonYoungErased=%llu "
        "scrubNonYoungNz=%llu env=MRT_GCV2_O2OREMSET=1",
        point == nullptr ? "?" : point, static_cast<unsigned long long>(obj),
        static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(young),
        static_cast<unsigned long long>(reg), static_cast<unsigned long long>(remSum),
        static_cast<unsigned long long>(remNz), static_cast<unsigned long long>(o2y),
        static_cast<unsigned long long>(scrubCalls), static_cast<unsigned long long>(scrubErased),
        static_cast<unsigned long long>(scrubNz));
    // Positive control: youngObjFwd must be non-zero when minor evacuates; proves probe armed.
    if (reset) {
        g_oldObjFwd.store(0, std::memory_order_relaxed);
        g_oldObjFwdBytes.store(0, std::memory_order_relaxed);
        g_youngObjFwd.store(0, std::memory_order_relaxed);
        g_oldRegionFwd.store(0, std::memory_order_relaxed);
        g_remsetInFromSum.store(0, std::memory_order_relaxed);
        g_remsetInFromNzRegions.store(0, std::memory_order_relaxed);
        g_o2yOnToSum.store(0, std::memory_order_relaxed);
        g_scrubNonYoungCalls.store(0, std::memory_order_relaxed);
        g_scrubNonYoungErased.store(0, std::memory_order_relaxed);
        g_scrubNonYoungNz.store(0, std::memory_order_relaxed);
        g_sampleLeft.store(16, std::memory_order_relaxed);
    }
}

} // namespace O2ORemsetDiag
} // namespace MapleRuntime
