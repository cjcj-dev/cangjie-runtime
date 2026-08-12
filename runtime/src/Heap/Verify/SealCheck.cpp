// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/SealCheck.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/Panic.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace SealCheck {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

bool GateOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_SEALCHECK");
    return on;
}

bool InjectOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_SEALCHECK_INJECT");
    return on;
}

bool FatalOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_VERIFY_SEALCHECK_FATAL");
    return on;
}

size_t MaxSamples()
{
    static const size_t n = EnvSizeT("MRT_GCV2_VERIFY_SEALCHECK_MAX", 64);
    return n;
}

std::atomic<size_t> g_sealedRegions{ 0 };
std::atomic<size_t> g_latePaint{ 0 };
std::atomic<size_t> g_logged{ 0 };
std::atomic<size_t> g_injectDone{ 0 };
std::atomic<bool> g_atexit{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { DumpSummary(); });
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteSeal(RegionInfo* region)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    if (!region->IsMarkFaceSealed()) {
        region->SetMarkFaceSealed(true);
        size_t n = g_sealedRegions.fetch_add(1, std::memory_order_relaxed) + 1;
        MaybeInjectLatePaint(region);
        // Periodic summary so timeout-killed ALOT runs still emit SEALCHECK line.
        if (n == 1 || (n % 256) == 0) {
            DumpSummary();
        }
    }
}

void NotePaint(RegionInfo* region, size_t offset, size_t byteCnt, const char* site)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    if (!region->IsMarkFaceSealed()) {
        return;
    }
    EnsureAtexit();
    size_t n = g_latePaint.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t logged = g_logged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logged <= MaxSamples()) {
        unsigned phase = static_cast<unsigned>(Heap::GetHeap().GetGCPhase());
        unsigned rs = static_cast<unsigned>(region->GetRouteState());
        LOG(RTLOG_ERROR,
            "[GCV2][sealcheck][LATE_PAINT] n=%zu region=%p start=%#zx offset=%zu byteCnt=%zu "
            "site=%s routeState=%u phase=%u young=%u",
            n, region, static_cast<size_t>(region->GetRegionStart()), offset, byteCnt,
            site != nullptr ? site : "?", rs, phase,
            static_cast<unsigned>(region->IsYoungRegion()));
    }
    if (FatalOn()) {
        CHECK_DETAIL(false,
                     "MRT_GCV2_VERIFY_SEALCHECK_FATAL: late paint after seal site=%s region=%p offset=%zu",
                     site != nullptr ? site : "?", region, offset);
    }
}

void MaybeInjectLatePaint(RegionInfo* region)
{
    if (!GateOn() || !InjectOn() || region == nullptr) {
        return;
    }
    size_t prev = g_injectDone.fetch_add(1, std::memory_order_relaxed);
    if (prev != 0) {
        return;
    }
    // Positive control: trip NotePaint after seal (proves checker rings).
    // Do NOT paint the product mark face — that would shift GetRoute prefix-sum
    // geometry for every later object in the region (the very contract this
    // instrument is supposed to detect, not create).
    constexpr size_t kInjectOff = 8;
    constexpr size_t kInjectSz = 8;
    LOG(RTLOG_ERROR,
        "[GCV2][sealcheck][INJECT] painting after seal region=%p offset=%zu", region, kInjectOff);
    NotePaint(region, kInjectOff, kInjectSz, "SEALCHECK_INJECT");
    LOG(RTLOG_ERROR,
        "[GCV2][sealcheck][INJECT] notePaint_done checker_only region=%p (no product MarkBits)",
        region);
}

void DumpSummary()
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR, "SEALCHECK sealed_regions=%zu late_paint=%zu inject=%u",
        g_sealedRegions.load(std::memory_order_relaxed),
        g_latePaint.load(std::memory_order_relaxed),
        static_cast<unsigned>(InjectOn()));
}

} // namespace SealCheck
} // namespace MapleRuntime
