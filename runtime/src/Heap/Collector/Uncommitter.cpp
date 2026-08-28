// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Uncommitter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/TimeUtils.h"

namespace MapleRuntime {
namespace {
std::atomic<bool> g_cycleCanceled{ false };

bool EnvIsSet(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}
} // namespace

uint64_t Uncommitter::ParseDelayNs(const char* env)
{
    if (env == nullptr) {
        return kDefaultDelayNs;
    }
    CString raw(env);
    CString s = raw.RemoveBlankSpace();
    if (s.Length() == 0) {
        return kDefaultDelayNs;
    }
    s.ToLowerCase();
    if (s == "0" || s == "0s" || s == "0ms" || s == "0us" || s == "0ns") {
        return 0;
    }
    uint64_t parsed = CString::ParseTimeFromEnv(s);
    if (parsed > 0) {
        return parsed;
    }
    if (CString::IsPosNumber(s)) {
        return std::strtoul(s.Str(), nullptr, 0) * SECOND_TO_NANO_SECOND;
    }
    LOG(RTLOG_ERROR,
        "Unsupported cjUncommitDelay parameter. Use 0 to disable, or a duration with unit (ns/us/ms/s). "
        "Default is 300s.");
    return kDefaultDelayNs;
}

uint64_t Uncommitter::DelayNs()
{
    static const uint64_t delayNs = ParseDelayNs(std::getenv("cjUncommitDelay"));
    return delayNs;
}

uint64_t Uncommitter::ComputeTickNs(uint64_t delayNs)
{
    if (delayNs == 0) {
        return 0;
    }
    uint64_t tenth = delayNs / 10;
    return tenth == 0 ? delayNs : std::min(tenth, kMaxTickNs);
}

uint64_t Uncommitter::TickNs()
{
    return ComputeTickNs(DelayNs());
}

uint32_t Uncommitter::TickMs()
{
    uint64_t tickNs = TickNs();
    if (tickNs == 0) {
        return 0;
    }
    uint64_t tickMs = tickNs / MILLI_SECOND_TO_NANO_SECOND;
    return tickMs == 0 ? 1 : static_cast<uint32_t>(tickMs);
}

size_t Uncommitter::ChunkLimit(size_t maxCapacity)
{
    size_t granule = MRT_PAGE_SIZE == 0 ? 4096 : MRT_PAGE_SIZE;
    size_t byCapacity = (maxCapacity >> 7);
    if (byCapacity < granule) {
        byCapacity = granule;
    } else {
        byCapacity = RoundUp(byCapacity, granule);
    }
    return std::min(byCapacity, kMaxUncommitChunk);
}

size_t Uncommitter::MinCapacity(size_t liveBytes, size_t youngReserve)
{
    size_t sum = liveBytes + youngReserve;
    if (sum < liveBytes) {
        return static_cast<size_t>(-1);
    }
    return sum;
}

size_t Uncommitter::FlushBytes(size_t usedBytes, size_t dirtyBytes, size_t minCapacity, size_t chunkLimit)
{
    if (dirtyBytes == 0 || chunkLimit == 0) {
        return 0;
    }
    size_t committed = usedBytes + dirtyBytes;
    if (committed < usedBytes) {
        return 0;
    }
    if (committed <= minCapacity) {
        return 0;
    }
    size_t release = committed - minCapacity;
    return std::min({ release, dirtyBytes, chunkLimit });
}

void Uncommitter::ActivateCycle()
{
    g_cycleCanceled.store(false, std::memory_order_release);
}

void Uncommitter::CancelCycle()
{
    g_cycleCanceled.store(true, std::memory_order_release);
}

bool Uncommitter::CycleCanceled()
{
    return g_cycleCanceled.load(std::memory_order_acquire);
}

bool Uncommitter::CutCancelWake()
{
#if defined(MRT_GC_UNIT_TESTS)
    return EnvIsSet("MRT_UNCOMMIT_CUT_CANCEL");
#else
    return false;
#endif
}

bool Uncommitter::CutCacheOwnership()
{
#if defined(MRT_GC_UNIT_TESTS)
    return EnvIsSet("MRT_UNCOMMIT_CUT_OWNERSHIP");
#else
    return false;
#endif
}

bool Uncommitter::CutPartialPropagation()
{
#if defined(MRT_GC_UNIT_TESTS)
    return EnvIsSet("MRT_UNCOMMIT_CUT_PARTIAL");
#else
    return false;
#endif
}

bool Uncommitter::ShouldStopUncommit()
{
    if (CutCancelWake()) {
        return false;
    }
    return CycleCanceled();
}

size_t Uncommitter::AccountReleased(size_t requestedBytes, size_t releasedBytes)
{
    if (CutPartialPropagation()) {
        return requestedBytes;
    }
    return releasedBytes;
}

bool Uncommitter::ShouldRetryPartial(size_t requestedBytes, size_t releasedBytes)
{
    if (CutPartialPropagation()) {
        return false;
    }
    return releasedBytes < requestedBytes;
}
} // namespace MapleRuntime
