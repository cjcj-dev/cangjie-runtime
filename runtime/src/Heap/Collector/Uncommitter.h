// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_UNCOMMITTER_H
#define MRT_UNCOMMITTER_H

#include <cstddef>
#include <cstdint>

#include "Base/Globals.h"

namespace MapleRuntime {
class Uncommitter {
public:
    static constexpr uint64_t kDefaultDelayNs = 300ULL * SECOND_TO_NANO_SECOND;
    static constexpr uint64_t kMaxTickNs = 30ULL * SECOND_TO_NANO_SECOND;
    static constexpr size_t kMaxUncommitChunk = 256 * MB;

    static uint64_t DelayNs();
    static uint64_t TickNs();
    static uint32_t TickMs();
    static bool Enabled() { return DelayNs() > 0; }

    static uint64_t ComputeTickNs(uint64_t delayNs);
    static size_t ChunkLimit(size_t maxCapacity);
    static size_t MinCapacity(size_t liveBytes, size_t youngReserve);
    static size_t FlushBytes(size_t usedBytes, size_t dirtyBytes, size_t minCapacity, size_t chunkLimit);

    static uint64_t ParseDelayNs(const char* env);
};
} // namespace MapleRuntime
#endif // MRT_UNCOMMITTER_H
