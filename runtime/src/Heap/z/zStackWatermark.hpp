// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#ifndef MRT_STACK_WATERMARK_H
#define MRT_STACK_WATERMARK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "Base/Log.h"
#include "Common/StackType.h"
#include "Common/BaseObject.h"
#include "StackMap/StackMapTypeDef.h"
#include "Heap/z/zUncoloredRoot.hpp"

namespace MapleRuntime {

class Mutator;
class AllocBuffer;
struct FrameInfo;

struct ThreadLocalAllocStats {
    size_t retired = 0;
};

class StackWatermarkProcessOopClosure {
public:
    using RootFunction = void (*)(zaddress_unsafe*, uintptr_t);
    static RootFunction select_function(void* context);
    StackWatermarkProcessOopClosure(void* context, uintptr_t color);
    void do_root(zaddress_unsafe* p);

private:
    RootFunction function;
    uintptr_t color;
};

class StackWatermark {
public:
    StackWatermark();

    void Reset()
    {
        state.store(0, std::memory_order_relaxed);
        cursorIndex.store(0, std::memory_order_relaxed);
        frameCount.store(0, std::memory_order_relaxed);
        stackGeneration.store(0, std::memory_order_relaxed);
        allocStats.retired = 0;
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
    void on_safepoint(Mutator& mutator);
    void start_processing(Mutator& mutator);

    bool TryBegin(uint64_t scanEpoch, size_t totalFrames);
    void AdvanceTo(size_t index) { cursorIndex.store(index, std::memory_order_release); }
    void Finish();
    void finish_processing() { Finish(); }

    uint64_t GetEpoch() const { return UnpackEpoch(state.load(std::memory_order_acquire)); }
    size_t GetCursorIndex() const { return cursorIndex.load(std::memory_order_acquire); }
    size_t GetFrameCount() const { return frameCount.load(std::memory_order_acquire); }
    uint64_t GetStackGeneration() const { return stackGeneration.load(std::memory_order_acquire); }
    uint32_t PackedState() const { return state.load(std::memory_order_acquire); }

    bool IsDone() const { return UnpackDone(state.load(std::memory_order_acquire)); }
    bool IsDone(uint64_t scanEpoch) const;

    void process_head(Mutator& mutator, void* context, const RootVisitor& visitor,
                      const RootVisitor& invisibleRootVisitor);
    bool start_processing_impl(Mutator& mutator, void* context, uint64_t epoch, size_t totalFrames,
                               const RootVisitor& visitor, const RootVisitor& invisibleRootVisitor);
    void process(const FrameInfo& frame, Mutator& mutator, void* context, const RootVisitor& visitor,
                 const DerivedPtrVisitor* derivedPtrVisitor, RegSlotsMap& regSlotsMap);
    ThreadLocalAllocStats& stats() { return allocStats; }

private:
    std::atomic<uint32_t> state;
    std::atomic<size_t> cursorIndex;
    std::atomic<size_t> frameCount;
    std::atomic<uint64_t> stackGeneration;
    ThreadLocalAllocStats allocStats;
};
class StackWatermarkSet {
public:
    static void on_safepoint(Mutator& mutator);
    static bool finish_processing(Mutator& mutator, const RootVisitor& visitor,
                                  const RootVisitor& invisibleRootVisitor, uint64_t epoch,
                                  const DerivedPtrVisitor* derivedPtrVisitor, size_t& scannedFrames,
                                  void* context = nullptr);
};

} // namespace MapleRuntime

#endif
