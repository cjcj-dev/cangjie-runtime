// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_WATERMARK_H
#define MRT_STACK_WATERMARK_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Base/Log.h"

namespace MapleRuntime {

// ZGC StackWatermarkState: packed (epoch << 1) | done plus a watermark cursor.
// Movable stacks keep a logical frame index and a generation (PLAN §5 infra).
class StackWatermark {
public:
    StackWatermark();

    void Reset()
    {
        state.store(0, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(0, std::memory_order_relaxed);
        stackGeneration.store(0, std::memory_order_relaxed);
    }

    void OnStackGrow(intptr_t stackOffset)
    {
        if (stackOffset == 0) {
            return;
        }
        (void)stackGeneration.fetch_add(1, std::memory_order_release);
    }

    static uint32_t PackState(uint32_t epochBits, bool done)
    {
        return (epochBits << 1) | (done ? 1u : 0u);
    }
    static uint32_t UnpackEpoch(uint32_t packed) { return packed >> 1; }
    static bool UnpackDone(uint32_t packed) { return (packed & 1u) != 0; }
    static uint32_t epoch_id();

    bool TryBegin(uint64_t scanEpoch, size_t totalFrames)
    {
        CHECK_DETAIL(scanEpoch != 0, "[GCV2][stack-watermark] epoch must not be zero");
        const uint32_t packed = state.load(std::memory_order_acquire);
        if (UnpackDone(packed) && UnpackEpoch(packed) == static_cast<uint32_t>(scanEpoch)) {
            return false;
        }
        if (!UnpackDone(packed) && UnpackEpoch(packed) == static_cast<uint32_t>(scanEpoch) && packed != 0) {
            return false;
        }
        uint32_t expected = packed;
        const uint32_t started = PackState(static_cast<uint32_t>(scanEpoch), false);
        if (!state.compare_exchange_strong(expected, started, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(totalFrames, std::memory_order_relaxed);
        return true;
    }

    void AdvanceTo(size_t index) { cursorIndex.store(index, std::memory_order_release); }

    void Finish()
    {
        const uint32_t packed = state.load(std::memory_order_relaxed);
        state.store(PackState(UnpackEpoch(packed), true), std::memory_order_release);
    }

    uint64_t GetEpoch() const { return UnpackEpoch(state.load(std::memory_order_acquire)); }
    size_t GetCursorIndex() const { return cursorIndex.load(std::memory_order_acquire); }
    size_t GetFrameCount() const { return frameCount.load(std::memory_order_acquire); }
    uint64_t GetStackGeneration() const { return stackGeneration.load(std::memory_order_acquire); }
    uint32_t PackedState() const { return state.load(std::memory_order_acquire); }

    bool IsDone() const { return UnpackDone(state.load(std::memory_order_acquire)); }
    bool IsDone(uint64_t scanEpoch) const;

private:
    std::atomic<uint32_t> state;
    std::atomic<size_t> cursorIndex;
    std::atomic<size_t> frameCount;
    std::atomic<uint64_t> stackGeneration;
};
} // namespace MapleRuntime

#endif // MRT_STACK_WATERMARK_H
