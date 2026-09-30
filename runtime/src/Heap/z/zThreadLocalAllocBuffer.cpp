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

void AllocBuffer::AccumulateTLABStatistics(TLABStatistics& total, size_t used, size_t capacity)
{
    const size_t requested = tlabStatistics.Used();
    if (requested != 0) {
        if (used > 0.5 * capacity) {
            tlabAllocationFraction.Sample(std::min(static_cast<double>(requested) /
                                                   std::max(capacity, size_t{1}), 1.0));
        }
        tlabStatistics.allocatingThreads = 1;
    }
    tlabStatistics.refills = tlabRefills.exchange(0, std::memory_order_relaxed);
    total.Update(tlabStatistics);
    tlabStatistics = TLABStatistics{};
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
    auto& manager = Heap::GetHeap().page_allocator();
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
    refillWasteLimit = InitialRefillWasteLimit();
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
    refillWasteLimit = InitialRefillWasteLimit();
}

// HotSpot threadLocalAllocBuffer.cpp:62-68. This runtime stores byte sizes.
size_t AllocBuffer::InitialRefillWasteLimit() const
{
    constexpr size_t refillWasteFraction = 64;
    // HotSpot divides in HeapWords; preserve that rounding in byte units.
    return (desiredTLABSize.load(std::memory_order_relaxed) / sizeof(uintptr_t) / refillWasteFraction) *
           sizeof(uintptr_t);
}

// HotSpot threadLocalAllocBuffer.cpp:68; convert HeapWords to byte units.
size_t AllocBuffer::RefillWasteLimitIncrement()
{
    return 4 * sizeof(uintptr_t);
}

// HotSpot threadLocalAllocBuffer.inline.hpp:90-97.
void AllocBuffer::RecordSlowAllocation(size_t objectSize)
{
    (void)objectSize;
    refillWasteLimit += RefillWasteLimitIncrement();
    ++tlabStatistics.slowAllocations;
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    (void)allocType;
    return AllocateInTLAB(totalSize);
}
} // namespace MapleRuntime
