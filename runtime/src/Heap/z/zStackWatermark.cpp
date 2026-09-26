// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/Mutator.h"
#include "UnwindStack/StackFrameCursor.h"

namespace MapleRuntime {

// runtime/stackWatermark.cpp:44-153. The stream owns its register locations,
// and caller/callee identify the two processed frames guarding the frontier.
class StackWatermarkFramesIterator {
public:
    explicit StackWatermarkFramesIterator(StackWatermark& owner)
        : owner(owner), cursor(owner.owner.GetUnwindContext()) {}
    bool has_next() const { return !cursor.Done(); }
    uintptr_t caller() const { return callerSP; }
    uintptr_t callee() const { return calleeSP; }
    void process_one(void* context)
    {
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            owner.process(frame, cursor.RegMap(), context);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
                break;
            }
        }
    }
    void process_all(void* context)
    {
        unsigned processed = 0;
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            owner.process(frame, cursor.RegMap(), context);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
                if (++processed == 5) {
                    processed = 0;
                    owner.yield_processing();
                }
            }
        }
    }
    // continuationFreezeThaw.cpp:595-598 flushes every frame that the copy will
    // publish. yield_processing (stackWatermark.cpp:225) would let a concurrent
    // watermark write land between the healed slot and the bytes that are copied.
    void process_all_no_yield(void* context)
    {
        while (has_next()) {
            const FrameInfo frame = *cursor.CurrentFrame();
            const bool barrier = has_barrier(frame);
            owner.process(frame, cursor.RegMap(), context);
            cursor.Advance();
            if (barrier) {
                set_watermark(frame.mFrame.GetSP());
            }
        }
    }
    void rebase(intptr_t offset)
    {
        if (callerSP != 0) { callerSP += offset; }
        if (calleeSP != 0) { calleeSP += offset; }
        cursor.Rebase(offset);
    }
private:
    static bool has_barrier(const FrameInfo& frame)
    {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
        // Other compiler targets have not yet supplied a return barrier ABI.
        return frame.GetFrameType() == FrameType::MANAGED && frame.mFrame.GetSP() != 0;
#else
        return false;
#endif
    }
    void set_watermark(uintptr_t sp)
    {
        if (!has_next()) { return; }
        if (calleeSP == 0) { calleeSP = sp; }
        else if (callerSP == 0) { callerSP = sp; }
        else { calleeSP = callerSP; callerSP = sp; }
    }
    StackWatermark& owner;
    StackFrameCursor cursor;
    uintptr_t callerSP = 0;
    uintptr_t calleeSP = 0;
};

uint32_t StackWatermark::epoch_id()
{
    return __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE);
}

StackWatermark::StackWatermark(Mutator& thread) : owner(thread), state(PackState(epoch_id(), true)) {}
StackWatermark::~StackWatermark() = default;

void StackWatermark::Reset()
{
    iterator.reset();
    waterMark.store(0, std::memory_order_relaxed);
    state.store(PackState(epoch_id(), true), std::memory_order_relaxed);
}

void StackWatermark::OnStackGrow(intptr_t offset)
{
    if (offset == 0) { return; }
    if (iterator != nullptr) { iterator->rebase(offset); }
    const uintptr_t old = watermark();
    if (old != 0) { waterMark.store(old + offset, std::memory_order_release); }
}

void StackWatermark::BeginGrowFlush()
{
    lock.lock();
    if (!processing_started()) {
        start_processing_impl(nullptr);
    }
    if (!IsDone() && iterator != nullptr) {
        iterator->process_all_no_yield(nullptr);
        update_watermark();
    }
}

void StackWatermark::EndGrowFlush()
{
    lock.unlock();
}

void StackWatermark::ShiftForGrow(intptr_t offset)
{
    OnStackGrow(offset);
}

uintptr_t StackWatermark::last_processed_raw() const
{
    return iterator == nullptr ? 0 : iterator->caller();
}

void StackWatermark::update_watermark()
{
    if (iterator != nullptr && iterator->has_next()) {
        waterMark.store(iterator->callee(), std::memory_order_release);
        state.store(PackState(epoch_id(), false), std::memory_order_release);
    } else {
        waterMark.store(0, std::memory_order_release);
        state.store(PackState(epoch_id(), true), std::memory_order_release);
    }
}

void StackWatermark::start_processing_impl(void* context)
{
    iterator.reset();
    if (owner.IsManagedContext()) {
        iterator.reset(new StackWatermarkFramesIterator(*this));
        // runtime/stackWatermark.cpp:205-228: callee, caller, unwind margin.
        iterator->process_one(context);
        iterator->process_one(context);
        iterator->process_one(context);
    }
    update_watermark();
}

void StackWatermark::yield_processing()
{
    update_watermark();
    lock.unlock();
    lock.lock();
}

void StackWatermark::start_processing()
{
    if (processing_started()) { return; }
    lock.lock();
    if (!processing_started()) { start_processing_impl(nullptr); }
    lock.unlock();
}

void StackWatermark::finish_processing(void* context)
{
    lock.lock();
    if (!processing_started()) { start_processing_impl(context); }
    if (!IsDone()) {
        iterator->process_all(context);
        update_watermark();
    }
    lock.unlock();
}

void StackWatermark::process_one()
{
    lock.lock();
    if (!processing_started()) { start_processing_impl(nullptr); }
    else if (!IsDone()) {
        iterator->process_one(nullptr);
        update_watermark();
    }
    lock.unlock();
}

bool StackWatermark::is_frame_safe(const FrameInfo& frame) const
{
    if (!processing_started()) { return false; }
    if (IsDone()) { return true; }
    return frame.mFrame.GetSP() < iterator->caller();
}

void StackWatermark::ensure_safe(const FrameInfo& frame)
{
    if (IsDone(epoch_id())) { return; }
    // real_fp in HotSpot is the sender's SP, not the machine frame pointer.
    const uintptr_t senderSP = frame.CallerSP();
    const uintptr_t boundary = watermark();
    if (boundary != 0 && senderSP > boundary) { process_one(); }
}

void StackWatermark::on_safepoint() { start_processing(); }

namespace {
bool HasExposableFrame(Mutator& owner)
{
    if (!owner.IsManagedContext()) {
        return false;
    }
    const MachineFrame& top = owner.GetUnwindContext().frameInfo.mFrame;
    return top.GetFA() != nullptr && top.GetIP() != nullptr;
}
}

void StackWatermark::before_unwind()
{
    // stackWatermark.inline.hpp:86-106. Processing was started by on_safepoint
    // (javaThread.cpp:1112). A finished watermark has nothing to expose, and a
    // runtime leave has no Java frame: do not classify it.
    if (!processing_started() || IsDone() || !HasExposableFrame(owner)) {
        return;
    }
    StackFrameStream frames(&owner.GetUnwindContext());
    frames.Start();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (frames.IsDone()) { return; }
    frames.Next();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (!frames.IsDone()) { ensure_safe(frames.Current()); }
}

void StackWatermark::after_unwind()
{
    // stackWatermark.inline.hpp:109-124.
    if (!processing_started() || IsDone() || !HasExposableFrame(owner)) {
        return;
    }
    StackFrameStream frames(&owner.GetUnwindContext());
    frames.Start();
    while (!frames.IsDone() && frames.Current().GetFrameType() != FrameType::MANAGED) { frames.Next(); }
    if (!frames.IsDone()) { ensure_safe(frames.Current()); }
}

void StackWatermark::on_iteration(const FrameInfo& frame) { ensure_safe(frame); }

StackWatermarkProcessOopClosure::RootFunction StackWatermarkProcessOopClosure::select_function(void* context)
{
    return context == nullptr ? ZUncoloredRoot::process : reinterpret_cast<RootFunction>(context);
}
StackWatermarkProcessOopClosure::StackWatermarkProcessOopClosure(void* context, uintptr_t color)
    : function(select_function(context)), color(color) {}
void StackWatermarkProcessOopClosure::do_root(zaddress_unsafe* p) { function(p, color); }

bool ZColorWatermark::covers(const ZColorWatermark& other) const
{
    if (watermark == 0) { return true; }
    if (other.watermark == 0) { return false; }
    return watermark >= other.watermark;
}

ZStackWatermark::ZStackWatermark(Mutator& owner) : StackWatermark(owner) { Reset(); }
void ZStackWatermark::Reset()
{
    StackWatermark::Reset();
    oldWatermarks[0] = { ZPointerStoreBadMask, 1 };
    oldWatermarks[1] = {};
    oldWatermarks[2] = {};
    newest = 0;
    allocStats = TLABStatistics{};
}
uintptr_t ZStackWatermark::prev_head_color() const { return oldWatermarks[newest].color; }
uintptr_t ZStackWatermark::prev_frame_color(const FrameInfo& frame) const
{
    for (int i = newest; i >= 0; --i) {
        if (oldWatermarks[i].watermark == 0 || frame.mFrame.GetSP() <= oldWatermarks[i].watermark) {
            return oldWatermarks[i].color;
        }
    }
    LOG(RTLOG_FATAL, "Found no matching previous color for the frame");
    return 0;
}

void ZStackWatermark::save_old_watermark()
{
    const uintptr_t previousColor = GetEpoch();
    if (previousColor == prev_head_color()) { return; }
    const ZColorWatermark previous { previousColor, IsDone() ? 0 : last_processed_raw() };
    int replace = -1;
    for (int i = 0; i <= newest; ++i) {
        if (previous.covers(oldWatermarks[i])) { replace = i; break; }
    }
    newest = replace == -1 ? newest + 1 : replace;
    CHECK_DETAIL(newest < OldWatermarksMax, "Unexpected amount of old watermarks");
    oldWatermarks[newest] = previous;
}

void ZStackWatermark::process_head(void* context)
{
    StackWatermarkProcessOopClosure closure(context, prev_head_color());
    RootVisitor roots = [&](RootSlot& root) {
        owner.VisitHeapRootSlots(root, [&](RootSlot& slot) {
            closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
        });
    };
    owner.VisitExceptionRoots(roots);
    owner.VisitNativeFrameRoots(roots);
    zaddress_unsafe* invisible = owner.GetGCData().invisibleRoot;
    if (invisible != nullptr) { ZUncoloredRoot::process_invisible(invisible, prev_head_color()); }
}

void ZStackWatermark::start_processing_impl(void* context)
{
    save_old_watermark();
    process_head(context);
    owner.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
    if ((ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark()) ||
        (ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_mark())) {
        ZThreadLocalAllocBuffer::retire(owner, allocStats);
    }
    if (owner.GetGCData().storeBarrierBuffer != nullptr) { owner.GetGCData().storeBarrierBuffer->on_new_phase(); }
    StackWatermark::start_processing_impl(context);
}

void ZStackWatermark::process(const FrameInfo& frame, RegSlotsMap& registers, void* context)
{
    StackWatermarkProcessOopClosure closure(context, prev_frame_color(frame));
    RootVisitor roots = [&](RootSlot& root) {
        owner.VisitHeapRootSlots(root, [&](RootSlot& slot) {
            closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
        });
    };
    DerivedPtrVisitor derived = Mutator::MakeDerivedRootVisitor(roots);
    StackFrameCursor::ProcessFrame(frame, registers, roots, owner, &derived, false);
}

void ZStackWatermark::ShiftForGrow(intptr_t offset)
{
    StackWatermark::OnStackGrow(offset);
    for (int i = 0; i <= newest; ++i) {
        if (oldWatermarks[i].watermark > 1) { oldWatermarks[i].watermark += offset; }
    }
}

void ZStackWatermark::OnStackGrow(intptr_t offset)
{
    std::lock_guard<std::mutex> guard(lock);
    ShiftForGrow(offset);
}

void StackWatermarkSet::on_safepoint(Mutator& mutator) { mutator.GetStackWatermark().on_safepoint(); }
void StackWatermarkSet::start_processing(Mutator& mutator) { mutator.GetStackWatermark().start_processing(); }
void StackWatermarkSet::finish_processing(Mutator& mutator, void* context)
{
    mutator.GetStackWatermark().finish_processing(context);
}
void StackWatermarkSet::before_unwind(Mutator& mutator)
{
    mutator.GetStackWatermark().before_unwind();
    UpdatePollValues(ThreadLocal::GetThreadLocalData());
}
void StackWatermarkSet::after_unwind(Mutator& mutator)
{
    mutator.GetStackWatermark().after_unwind();
    UpdatePollValues(ThreadLocal::GetThreadLocalData());
}
void StackWatermarkSet::on_iteration(Mutator& mutator, const FrameInfo& frame)
{
    mutator.GetStackWatermark().on_iteration(frame);
}
uintptr_t StackWatermarkSet::lowest_watermark(Mutator& mutator) { return mutator.GetStackWatermark().watermark(); }
} // namespace MapleRuntime
