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
} // namespace MapleRuntime
