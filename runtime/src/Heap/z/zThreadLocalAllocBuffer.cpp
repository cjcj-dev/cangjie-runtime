// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Allocator/RegionSpace.h"
#include "Base/MemUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Mutator/Mutator.h"
#include "Heap/z/zValue.inline.hpp"
namespace MapleRuntime {
ZPerWorker<TLABStatistics>* ZThreadLocalAllocBuffer::statistics = nullptr;

void ZThreadLocalAllocBuffer::initialize()
{
    delete statistics;
    statistics = new ZPerWorker<TLABStatistics>();
    reset_statistics();
}

void ZThreadLocalAllocBuffer::reset_statistics()
{
    statistics->set_all(TLABStatistics{});
}

void ZThreadLocalAllocBuffer::publish_statistics()
{
    TLABStatistics total;
    ZPerWorkerIterator<TLABStatistics> iter(statistics);
    for (TLABStatistics* stats; iter.next(&stats);) { total.Update(*stats); }
    Heap::GetHeap().page_allocator().PublishTLABStatistics(total);
}

void ZThreadLocalAllocBuffer::retire(Mutator& thread, TLABStatistics& stats)
{
    stats = TLABStatistics{};
    Heap::GetHeap().page_allocator().RetireTLAB(*thread.tlab(), stats);
}

void ZThreadLocalAllocBuffer::update_stats(Mutator& thread)
{
    statistics->addr()->Update(thread.GetStackWatermark().stats());
}

constexpr size_t AllocBuffer::MinTLABSize;

AllocBuffer* AllocBuffer::GetAllocBuffer()
{
    Mutator* owner = ThreadLocal::GetMutator();
    return owner != nullptr ? owner->tlab() : nullptr;
}

AllocBuffer::~AllocBuffer()
{
    FlushRegion();
}

void AllocBuffer::Init()
{
    if (initialized) { return; }
    initialized = true;
    static_assert(offsetof(AllocBuffer, tlab) == 0, "compiler TLAB inline ABI");
    static_assert(offsetof(TLAB, top) == 0, "compiler TLAB top ABI");
    static_assert(offsetof(TLAB, end) == 8, "compiler TLAB end ABI");
    tlab = TLAB{};
    ThreadLocal::InitializeCleaner();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.InitializeTLAB(*this);
}

void AllocBuffer::Fini()
{
    if (!initialized) { return; }
    initialized = false;
    // Mark stacks and store buffers are flushed by on_thread_detach.
    FlushRegion();
    // JavaThread::exit calls retire_tlab() without a statistics destination
    // (javaThread.cpp:831). Only root workers contribute watermark snapshots
    // to the cycle totals; exiting threads do not write a shared accumulator.
}

// ThreadLocalAllocBuffer::fill (threadLocalAllocBuffer.cpp:201).
void AllocBuffer::FillTLAB(uintptr_t start, size_t size)
{
    tlab.start = start;
    tlab.top = start;
    tlab.end = start + size;
    tlabStatistics.allocatedSize += size;
    tlabRefills.fetch_add(1, std::memory_order_relaxed);
}

ZPage* AllocBuffer::GetRegion() const
{
    return tlab.start == 0 ? nullptr : Heap::page(tlab.start);
}

uintptr_t AllocBuffer::AllocateInTLAB(size_t size)
{
    if (size > tlab.end - tlab.top) { return 0; }
    const uintptr_t result = tlab.top;
    tlab.top += size;
    return result;
}

// threadLocalAllocBuffer.cpp:130-158: make the unused tail parsable before retirement.
void AllocBuffer::RetireTLAB(bool gcWaste)
{
    if (tlab.start == 0) { return; }
    const size_t waste = tlab.end - tlab.top;
    CollectedHeap::fill_with_dummy_object(tlab.top, tlab.top + waste, true);
    if (gcWaste) { tlabStatistics.gcWaste += waste; }
    else { tlabStatistics.refillWaste += waste; }
    tlab = TLAB{};
}

void AllocBuffer::ClearRegion() { RetireTLAB(true); }
void AllocBuffer::FlushRegion() { RetireTLAB(true); }

// threadLocalAllocBuffer.inline.hpp:57-91, byte units in this runtime.
size_t AllocBuffer::ComputeTLABSize(size_t objectSize, size_t maxSize) const
{
    if (objectSize > maxSize || maxSize < MinTLABSize) { return 0; }
    const size_t desired = desiredTLABSize.load(std::memory_order_relaxed);
    const size_t size = desired > maxSize - objectSize ? maxSize : desired + objectSize;
    return AlignUp(size, size_t{8});
}

void AllocBuffer::ResizeTLAB(size_t capacity, double fallbackFraction, size_t maxSize)
{
    constexpr size_t targetRefills = 50;
    double fraction = tlabAllocationFraction.Average();
    if (fraction == 0) { fraction = fallbackFraction; }
    const size_t allocation = static_cast<size_t>(fraction * capacity);
    const size_t desired = std::min(std::max(allocation / targetRefills, MinTLABSize), maxSize);
    desiredTLABSize.store(AlignUp(desired, size_t{8}), std::memory_order_relaxed);
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    (void)allocType;
    return AllocateInTLAB(totalSize);
}

MAddress AllocBuffer::AllocateImpl(size_t totalSize, AllocType allocType)
{
    (void)allocType;
    // HotSpot memAllocator.cpp:282-296: retire before computing the refill;
    // the caller owns the outside-TLAB fallback for every slow-path failure.
    RetireTLAB(false);
    const size_t tlabSize = ComputeTLABSize(totalSize, Heap::GetHeap().unsafe_max_tlab_alloc());
    if (tlabSize == 0) { return 0; }
    // Cangjie tasks can migrate while page allocation enters a saferegion.
    CJThreadPreemptOffCntAdd();
    size_t actualSize = 0;
    const uintptr_t start = ZCollectedHeap::heap()->allocate_new_tlab(totalSize, tlabSize, &actualSize);
    CJThreadPreemptOffCntSub();
    if (start == 0) { return 0; }
    // HotSpot memAllocator.cpp:312-324: initialize the refill before publishing its bounds.
    MemorySet(start, actualSize, 0, actualSize);
    FillTLAB(start, actualSize);
    return AllocateInTLAB(totalSize);
}

} // namespace MapleRuntime
