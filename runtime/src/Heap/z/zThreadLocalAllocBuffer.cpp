// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Mutator/Mutator.h"
namespace MapleRuntime {
AllocBuffer* AllocBuffer::GetOrCreateAllocBuffer()
{
    auto* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBuffer();
        CHECK_DETAIL(buffer != nullptr, "new region alloc buffer fail");
        buffer->Init();
        ThreadLocal::SetAllocBuffer(buffer);
        RegisterCurrentMarkFlushThread();
    }
    return buffer;
}

AllocBuffer* AllocBuffer::GetAllocBuffer() { return ThreadLocal::GetAllocBuffer(); }

AllocBuffer::~AllocBuffer()
{
    FlushRegion();
}

void AllocBuffer::Init()
{
    static_assert(offsetof(AllocBuffer, tlab) == 0, "compiler TLAB inline ABI");
    static_assert(offsetof(TLAB, top) == 0, "compiler TLAB top ABI");
    static_assert(offsetof(TLAB, end) == 8, "compiler TLAB end ABI");
    tlab = TLAB{};
    ThreadLocal::InitializeCleaner();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.InitializeTLAB(*this);
    Heap::GetHeap().RegisterAllocBuffer(*this);
}

void AllocBuffer::Fini()
{
    // Finish allocation publications before releasing the current context.
    // Mark stacks and SBB remain owned by the OS thread until its detach.
    if (ThreadLocal::GetAllocBuffer() == this) {
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
    FlushRegion();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.RetireTLABStatistics(*this);
    Heap::GetHeap().RemoveAllocBuffer(*this);
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
    if (UNLIKELY(allocType == AllocType::RAW_POINTER_OBJECT)) {
        return AllocateRawPointerObject(totalSize);
    }
    const uintptr_t addr = AllocateInTLAB(totalSize);
    return addr != 0 ? addr : AllocateImpl(totalSize, allocType);
}

MAddress AllocBuffer::AllocateImpl(size_t totalSize, AllocType allocType)
{
    (void)allocType;
    const size_t tlabSize = ComputeTLABSize(totalSize, Heap::GetHeap().unsafe_max_tlab_alloc());
    if (tlabSize == 0) {
        return Heap::GetHeap().object_allocator().alloc(totalSize, PageAge::eden);
    }
    RetireTLAB(false);
    // Cangjie tasks can migrate while page allocation enters a saferegion.
    CJThreadPreemptOffCntAdd();
    size_t actualSize = 0;
    const uintptr_t start = ZCollectedHeap::heap()->allocate_new_tlab(totalSize, tlabSize, &actualSize);
    CJThreadPreemptOffCntSub();
    if (start == 0) { return 0; }
    FillTLAB(start, actualSize);
    return AllocateInTLAB(totalSize);
}

MAddress AllocBuffer::AllocateRawPointerObject(size_t totalSize)
{
    ZPage* region = tlRawPointerRegions.empty() ? nullptr : tlRawPointerRegions.back();
    if (region != nullptr) {
        MAddress allocAddr = region->alloc_object(totalSize);
        if (allocAddr != 0) {
            return allocAddr;
        }
    }
    size_t pageSize = AlignUp(totalSize, ZGranuleSize);
    if (totalSize <= ZObjectSizeLimitSmall) {
        region = Heap::alloc_page(pageSize, ZPageType::small);
        if (region == nullptr) {
            return 0;
        }
        region->SetRegionRole(ZPageRole::RawPointerStaging);
        tlRawPointerRegions.push_back(region);
    } else {
        region = Heap::alloc_page(pageSize, ZPageType::large);
        if (region == nullptr) {
            return 0;
        }
        region->SetRegionRole(ZPageRole::RawPointerStaging);
        tlLargeRawPointerRegions.push_back(region);
    }

    // region is enough for totalSize.
    MAddress allocAddr = region->alloc_object(totalSize);
    MRT_ASSERT(allocAddr != 0, "allocation failure");
    return allocAddr;
}

void AllocBuffer::CommitRawPointerRegions()
{
    Heap::GetHeap().page_allocator().MergeRawPointerRegions(tlRawPointerRegions, tlLargeRawPointerRegions);
}
} // namespace MapleRuntime
