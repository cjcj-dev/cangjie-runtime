// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zMetronome.hpp:30-43. Host infra difference (PLAN §5): HotSpot Monitor
// (rank/safepoint protocol) has no counterpart here; ZConditionLock is the
// same-layer primitive (zLock.hpp I14 note).
#ifndef MRT_GC_Z_ZMETRONOME_HPP
#define MRT_GC_Z_ZMETRONOME_HPP

#include <cstdint>

#include "Heap/z/zLock.hpp"

namespace MapleRuntime {
class ZMetronome {
private:
    ZConditionLock _lock;
    const uint64_t _intervalMs;
    uint64_t _startMs;
    uint64_t _nticks;
    bool _stopped;

public:
    explicit ZMetronome(uint64_t hz);

    bool wait_for_tick();
    void stop();
};
} // namespace MapleRuntime
#endif // MRT_GC_Z_ZMETRONOME_HPP
