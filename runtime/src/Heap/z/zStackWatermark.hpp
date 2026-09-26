// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#ifndef MRT_STACK_WATERMARK_H
#define MRT_STACK_WATERMARK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include "Common/StackType.h"
#include "Common/BaseObject.h"
#include "StackMap/StackMapTypeDef.h"
#include "Heap/z/zUncoloredRoot.hpp"
#include "Heap/z/zTLABUsage.hpp"

namespace MapleRuntime {
class Mutator;
class StackWatermarkFramesIterator;

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

// HotSpot runtime/stackWatermark.hpp: state publication and frame iteration.
class StackWatermark {
public:
    explicit StackWatermark(Mutator& owner);
    virtual ~StackWatermark();
    static uint32_t PackState(uint32_t epoch, bool done) { return (epoch << 1) | (done ? 1u : 0u); }
    static uint32_t UnpackEpoch(uint32_t packed) { return packed >> 1; }
    static bool UnpackDone(uint32_t packed) { return (packed & 1u) != 0; }
    static uint32_t epoch_id();
    uint32_t PackedState() const { return state.load(std::memory_order_acquire); }
    uint64_t GetEpoch() const { return UnpackEpoch(PackedState()); }
    bool IsDone() const { return UnpackDone(PackedState()); }
    bool IsDone(uint64_t epoch) const { return PackedState() == PackState(epoch, true); }
    bool processing_started() const { return GetEpoch() == epoch_id(); }
    uintptr_t watermark() const { return waterMark.load(std::memory_order_acquire); }
    uintptr_t last_processed_raw() const;
    void start_processing();
    void finish_processing(void* context);
    void on_safepoint();
    void before_unwind();
    void after_unwind();
    void on_iteration(const FrameInfo& frame);
    void ensure_safe(const FrameInfo& frame);
    bool is_frame_safe(const FrameInfo& frame) const;
    void process_one();
    virtual void Reset();
    virtual void OnStackGrow(intptr_t offset);

protected:
    virtual void start_processing_impl(void* context);
    virtual void process(const FrameInfo& frame, RegSlotsMap& registers, void* context) = 0;
    void update_watermark();
    void yield_processing();
    std::mutex lock;
    Mutator& owner;
    std::atomic<uint32_t> state;

private:
    std::atomic<uintptr_t> waterMark { 0 };
    std::unique_ptr<StackWatermarkFramesIterator> iterator;
    friend class StackWatermarkFramesIterator;
};

struct ZColorWatermark {
    uintptr_t color;
    uintptr_t watermark;
    bool covers(const ZColorWatermark& other) const;
};

// ZGC zStackWatermark.hpp: three overlapping historical colour intervals.
class ZStackWatermark final : public StackWatermark {
public:
    explicit ZStackWatermark(Mutator& owner);
    void Reset() override;
    void OnStackGrow(intptr_t offset) override;
    TLABStatistics& stats() { return allocStats; }
    uintptr_t prev_head_color() const;
    uintptr_t prev_frame_color(const FrameInfo& frame) const;

private:
    static constexpr int OldWatermarksMax = 3;
    ZColorWatermark oldWatermarks[OldWatermarksMax] {};
    int newest = 0;
    TLABStatistics allocStats;
    void save_old_watermark();
    void process_head(void* context);
    void start_processing_impl(void* context) override;
    void process(const FrameInfo& frame, RegSlotsMap& registers, void* context) override;
};

class StackWatermarkSet {
public:
    static void on_safepoint(Mutator& mutator);
    static void before_unwind(Mutator& mutator);
    static void after_unwind(Mutator& mutator);
    static void on_iteration(Mutator& mutator, const FrameInfo& frame);
    static void start_processing(Mutator& mutator);
    static void finish_processing(Mutator& mutator, void* context = nullptr);
    static uintptr_t lowest_watermark(Mutator& mutator);
};
} // namespace MapleRuntime
#endif
