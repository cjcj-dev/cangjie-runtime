// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zMetronome.cpp:29-80. Host infra difference: Ticks/TimeHelper have no
// counterpart; the steady monotonic clock in milliseconds is the same datum.
#include "Heap/z/zMetronome.hpp"

#include <chrono>

#include "Heap/z/zLock.inline.hpp"

namespace MapleRuntime {
static uint64_t NowMillis()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

ZMetronome::ZMetronome(uint64_t hz) : _lock(), _intervalMs(1000 / hz), _startMs(0), _nticks(0), _stopped(false) {}

bool ZMetronome::wait_for_tick()
{
    if (_nticks++ == 0) {
        // First tick, set start time
        _startMs = NowMillis();
    }

    _lock.lock();
    while (!_stopped) {
        // We might wake up spuriously from wait, so always recalculate
        // the timeout after a wakeup to see if we need to wait again.
        const uint64_t nowMs = NowMillis();
        const uint64_t nextMs = _startMs + (_intervalMs * _nticks);
        const int64_t timeoutMs = static_cast<int64_t>(nextMs - nowMs);

        if (timeoutMs > 0) {
            // Wait
            _lock.wait(static_cast<uint64_t>(timeoutMs));
        } else {
            // Tick
            if (timeoutMs < 0) {
                const uint64_t overslept = static_cast<uint64_t>(-timeoutMs);
                if (overslept > _intervalMs) {
                    // Missed one or more ticks. Bump _nticks accordingly to
                    // avoid firing a string of immediate ticks to make up
                    // for the ones we missed.
                    _nticks += overslept / _intervalMs;
                }
            }
            _lock.unlock();
            return true;
        }
    }
    _lock.unlock();

    // Stopped
    return false;
}

void ZMetronome::stop()
{
    _lock.lock();
    _stopped = true;
    _lock.notify();
    _lock.unlock();
}
} // namespace MapleRuntime
