// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "UnwindStack/StackFrameCursor.h"

namespace MapleRuntime {

StackWatermark::StackWatermark() { Reset(); }

uint32_t StackWatermark::epoch_id()
{
    return __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE);
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

void StackWatermark::process_head(Mutator& mutator, void* context, const RootVisitor& visitor,
                                  const RootVisitor& invisibleRootVisitor)
{
    (void)context;
    mutator.VisitExceptionRoots(visitor);
    mutator.VisitNativeFrameRoots(visitor);
    mutator.VisitRawObjects(invisibleRootVisitor);
    zaddress_unsafe* invisible = mutator.GetGCData().invisibleRoot;
    if (invisible != nullptr) {
        const uintptr_t color = mutator.GetGCData().loadGoodMask != 0 ? mutator.GetGCData().loadGoodMask
                                                                     : ZPointerLoadGoodMask;
        ZUncoloredRoot::process_invisible(invisible, color);
    }
}

bool StackWatermark::start_processing_impl(Mutator& mutator, void* context, uint64_t epoch, size_t totalFrames,
                                           const RootVisitor& visitor, const RootVisitor& invisibleRootVisitor)
{
    if (!TryBegin(epoch, totalFrames)) {
        return false;
    }
    process_head(mutator, context, visitor, invisibleRootVisitor);
    mutator.GetGCData().InstallMasks(ThreadGCData::PublishedMasks());
    AllocBuffer* buffer = ThreadLocal::GetAllocBuffer();
    if (buffer != nullptr) {
        const bool youngMark = Heap::GetHeap().GetGCPhase(GCCycleGeneration::YOUNG) == GCPhase::GC_PHASE_ENUM ||
            Heap::GetHeap().GetGCPhase(GCCycleGeneration::YOUNG) == GCPhase::GC_PHASE_TRACE;
        const bool oldMark = Heap::GetHeap().GetGCPhase(GCCycleGeneration::OLD) == GCPhase::GC_PHASE_ENUM ||
            Heap::GetHeap().GetGCPhase(GCCycleGeneration::OLD) == GCPhase::GC_PHASE_TRACE;
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
                             const DerivedPtrVisitor* derivedPtrVisitor)
{
    (void)frame;
    (void)mutator;
    (void)context;
    (void)visitor;
    (void)derivedPtrVisitor;
}

} // namespace MapleRuntime
