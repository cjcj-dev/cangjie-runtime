// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "ObjectModel/MArray.h"
#include "Mutator/Mutator.h"
#include "UnwindStack/StackFrameCursor.h"

namespace MapleRuntime {

StackWatermark::StackWatermark() { Reset(); }

uint32_t StackWatermark::epoch_id()
{
    return __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE);
}

// HotSpot stackWatermarkSet.cpp:114 and stackWatermark.cpp:311.
void StackWatermarkSet::on_safepoint(Mutator& mutator)
{
    mutator.GetStackWatermark().on_safepoint(mutator);
}

void StackWatermark::on_safepoint(Mutator& mutator)
{
    start_processing(mutator);
}

void StackWatermark::start_processing(Mutator& mutator)
{
    const uint64_t epoch = epoch_id();
    if (epoch == 0 || IsDone(epoch)) { return; }
    RootVisitor visitor = [&](RootSlot& root) {
        StackWatermarkProcessOopClosure closure(nullptr, uncolored_root_color());
        mutator.VisitHeapRootSlots(root, [&](RootSlot& slot) {
            closure.do_root(reinterpret_cast<zaddress_unsafe*>(&slot));
        });
    };
    DerivedPtrVisitor derived = Mutator::MakeDerivedRootVisitor(visitor);
    size_t frames = 0;
    // Cangjie has no return statepoint (#498). Keep the existing eager frame
    // traversal; the no-frame head still uses the single start_processing_impl.
    (void)StackWatermarkSet::finish_processing(mutator, visitor, visitor, epoch, &derived, frames);
}

bool StackWatermark::IsDone(uint64_t scanEpoch) const
{
    const uint32_t packed = state.load(std::memory_order_acquire);
    return UnpackDone(packed) && UnpackEpoch(packed) == static_cast<uint32_t>(scanEpoch);
}

bool StackWatermark::TryBegin(uint64_t scanEpoch, size_t totalFrames)
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

void StackWatermark::Finish()
{
    const uint32_t packed = state.load(std::memory_order_relaxed);
    state.store(PackState(UnpackEpoch(packed), true), std::memory_order_release);
}

StackWatermarkProcessOopClosure::RootFunction StackWatermarkProcessOopClosure::select_function(void* context)
{
    if (context == nullptr) {
        return ZUncoloredRoot::process;
    }
    return reinterpret_cast<RootFunction>(context);
}

StackWatermarkProcessOopClosure::StackWatermarkProcessOopClosure(void* context, uintptr_t color)
    : function(select_function(context)), color(color)
{
}

void StackWatermarkProcessOopClosure::do_root(zaddress_unsafe* p)
{
    function(p, color);
}

void StackWatermark::save_old_watermark(Mutator& mutator)
{
    headColor = mutator.GetGCData().storeGoodMask;
}

void StackWatermark::process_head(Mutator& mutator, void* context, const RootVisitor& visitor,
                                  const RootVisitor& invisibleRootVisitor)
{
    (void)context;
    mutator.VisitExceptionRoots(visitor);
    mutator.VisitNativeFrameRoots(visitor);
    // ZGC zStackWatermark.cpp:164-174: the invisible slot is processed once,
    // below, with the saved head color. VisitRawObjects names that same slot.
    (void)invisibleRootVisitor;
    zaddress_unsafe* invisible = mutator.GetGCData().invisibleRoot;
    if (invisible != nullptr) {
        ZUncoloredRoot::process_invisible(invisible, uncolored_root_color());
#if defined(MRT_GC_UNIT_TESTS)
#endif
    }
}

bool StackWatermark::start_processing_impl(Mutator& mutator, void* context, uint64_t epoch, size_t totalFrames,
                                           const RootVisitor& visitor, const RootVisitor& invisibleRootVisitor)
{
    if (!TryBegin(epoch, totalFrames)) {
        return false;
    }
    save_old_watermark(mutator);
    process_head(mutator, context, visitor, invisibleRootVisitor);
    AllocBuffer* buffer = mutator.GetAllocBuffer();
    if (buffer != nullptr) {
        const bool youngMark = ZGeneration::young() != nullptr && ZGeneration::young()->is_phase_mark();
        const bool oldMark = ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_mark();
        if (youngMark || oldMark) {
            buffer->RetireTLAB(true);
            ++allocStats.retired;
        }
    }
    if (mutator.GetGCData().storeBarrierBuffer != nullptr) {
        mutator.GetGCData().storeBarrierBuffer->on_new_phase();
    }
    return true;
}

void StackWatermark::process(const FrameInfo& frame, Mutator& mutator, void* context, const RootVisitor& visitor,
                             const DerivedPtrVisitor* derivedPtrVisitor, RegSlotsMap& regSlotsMap)
{
    StackFrameCursor::ProcessFrame(frame, regSlotsMap, visitor, mutator, derivedPtrVisitor, false);
    (void)context;
}

bool StackWatermarkSet::finish_processing(Mutator& mutator, const RootVisitor& visitor,
                                          const RootVisitor& invisibleRootVisitor, uint64_t epoch,
                                          const DerivedPtrVisitor* derivedPtrVisitor, size_t& scannedFrames,
                                          void* context)
{
    scannedFrames = 0;
    mutator.MutatorLock();
    if (mutator.stackWatermark.IsDone(epoch)) {
        mutator.MutatorUnlock();
        return true;
    }
    if (!mutator.IsManagedContext()) {
        bool began = mutator.stackWatermark.start_processing_impl(mutator, context, epoch, 0, visitor,
                                                                  invisibleRootVisitor);
        if (began) {
            mutator.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
            mutator.stackWatermark.finish_processing();
        }
        mutator.MutatorUnlock();
        return began;
    }
    mutator.IncObserver();
    StackFrameCursor cursor(mutator.uwContext);
    bool began = mutator.stackWatermark.start_processing_impl(mutator, context, epoch, cursor.FrameCount(), visitor,
                                                              invisibleRootVisitor);
    if (began) {
        while (!cursor.Done()) {
            const FrameInfo* frame = cursor.CurrentFrame();
            if (frame != nullptr) {
                mutator.stackWatermark.process(*frame, mutator, context, visitor, derivedPtrVisitor,
                                               cursor.RegMap());
            }
            cursor.Advance();
            mutator.stackWatermark.AdvanceTo(cursor.Cursor());
        }
        scannedFrames = cursor.Cursor();
        mutator.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
        mutator.stackWatermark.finish_processing();
    }
    mutator.DecObserver();
    mutator.MutatorUnlock();
    return began;
}

} // namespace MapleRuntime
