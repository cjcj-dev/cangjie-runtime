// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "OopStorage.h"
#include <algorithm>

namespace MapleRuntime {
OopStorage::OopStorage() : activeArray(std::make_shared<ActiveArray>()) {}

OopStorage::~OopStorage()
{
    CHECK_DETAIL(concurrentIterationCount == 0, "storage destroyed during concurrent iteration");
    for (Block* block : activeArray->blocks) { delete block; }
}

NativeSlot* OopStorage::Block::Allocate()
{
    const uintptr_t mask = allocatedBitmask.load(std::memory_order_relaxed);
    const unsigned index = static_cast<unsigned>(__builtin_ctzl(~mask));
    allocatedBitmask.fetch_or(uintptr_t(1) << index, std::memory_order_relaxed);
    return &data[index];
}

size_t OopStorage::Block::Iterate(const NativeSlotVisitor& visitor)
{
    // oopStorage.inline.hpp:337: take the bitmap once for this block.
    uintptr_t remaining = allocatedBitmask.load(std::memory_order_relaxed);
    size_t visited = 0;
    while (remaining != 0) {
        const unsigned index = static_cast<unsigned>(__builtin_ctzl(remaining));
        remaining &= remaining - 1;
        visitor(data[index]);
        ++visited;
    }
    return visited;
}

void OopStorage::EnsureWritableArray()
{
    if (!activeArray.unique()) { activeArray = std::make_shared<ActiveArray>(*activeArray); }
}

NativeSlot* OopStorage::Allocate()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (allocationHead == nullptr) {
        std::unique_ptr<Block> block(new Block());
        EnsureWritableArray();
        block->activeIndex = activeArray->blocks.size();
        activeArray->blocks.push_back(block.get());
        AddAllocationBlock(*block.release());
    }
    Block& block = *allocationHead;
    NativeSlot* slot = block.Allocate();
    if (block.IsFull()) { RemoveAllocationBlock(block); }
    ++allocationCount;
    return slot;
}

void OopStorage::Release(NativeSlot* slot)
{
    std::lock_guard<std::mutex> lock(mutex);
    const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
    for (Block* block : activeArray->blocks) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(block->data.data());
        if (address < start || address >= start + sizeof(block->data)) { continue; }
        const size_t index = (address - start) / sizeof(Slot);
        const uintptr_t bit = uintptr_t(1) << index;
        CHECK_DETAIL((block->allocatedBitmask.load(std::memory_order_relaxed) & bit) != 0,
                     "release of inactive native slot");
        const bool wasFull = block->IsFull();
        // Native handle release publishes null before making the slot reusable.
        // An iterator with an older bitmap may still visit that null slot.
        slot->StoreColoured(zpointer::null, std::memory_order_relaxed);
        block->allocatedBitmask.fetch_and(~bit, std::memory_order_relaxed);
        --allocationCount;
        if (wasFull) { AddAllocationBlock(*block); }
        DeleteEmptyBlocks();
        return;
    }
    CHECK_DETAIL(false, "native slot belongs to another storage");
}

void OopStorage::DeleteEmptyBlocks()
{
    // oopStorage.cpp:981: every concurrent iterator pins the blocks, including
    // empty blocks in an older active array. Cangjie has no ServiceThread;
    // release and the last iteration completion perform deferred cleanup.
    if (concurrentIterationCount != 0) { return; }
    EnsureWritableArray();
    size_t index = 0;
    while (index < activeArray->blocks.size()) {
        Block* block = activeArray->blocks[index];
        if (block->allocatedBitmask.load(std::memory_order_relaxed) != 0) { ++index; continue; }
        RemoveAllocationBlock(*block);
        Block* last = activeArray->blocks.back();
        activeArray->blocks[index] = last;
        last->activeIndex = index;
        activeArray->blocks.pop_back();
        delete block;
    }
}

size_t OopStorage::AllocationCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return allocationCount;
}

size_t OopStorage::OopsDo(const NativeSlotVisitor& visitor)
{
    ParState<true> state(*this);
    return state.OopsDo(visitor);
}

OopStorage::BasicParState::BasicParState(OopStorage& storage, unsigned estimatedThreadCount, bool concurrent)
    : storage(storage), blockCount(0), estimatedThreadCount(estimatedThreadCount), concurrent(concurrent)
{
    CHECK_DETAIL(estimatedThreadCount > 0, "iteration requires a positive worker estimate");
    std::lock_guard<std::mutex> lock(storage.mutex);
    activeArray = storage.activeArray;
    if (concurrent) { ++storage.concurrentIterationCount; }
    blockCount = activeArray->blocks.size();
}

OopStorage::BasicParState::~BasicParState()
{
    std::lock_guard<std::mutex> lock(storage.mutex);
    activeArray.reset();
    if (concurrent) { --storage.concurrentIterationCount; }
    storage.DeleteEmptyBlocks();
}

bool OopStorage::BasicParState::ClaimNextSegment(IterationData& data)
{
    data.processed += data.segmentEnd - data.segmentStart;
    size_t start = nextBlock.load(std::memory_order_acquire);
    if (start >= blockCount) { return false; }
    // oopStorage.cpp:1088-1127: bound the claim before fetch-add and clamp
    // overshoot afterwards, so another worker can claim remaining segments.
    const size_t remaining = blockCount - start;
    const size_t step = std::min(size_t(10), 1 + remaining / estimatedThreadCount);
    start = nextBlock.fetch_add(step, std::memory_order_acq_rel);
    if (start >= blockCount) { return false; }
    data.segmentStart = start;
    data.segmentEnd = std::min(start + step, blockCount);
    return true;
}

size_t OopStorage::BasicParState::Iterate(const NativeSlotVisitor& visitor)
{
    IterationData data;
    size_t visited = 0;
    while (ClaimNextSegment(data)) {
        for (size_t index = data.segmentStart; index < data.segmentEnd; ++index) {
            visited += activeArray->blocks[index]->Iterate(visitor);
        }
    }
    return visited;
}

void OopStorage::AddAllocationBlock(Block& block)
{
    block.allocationPrev = nullptr;
    block.allocationNext = allocationHead;
    if (allocationHead != nullptr) { allocationHead->allocationPrev = &block; }
    allocationHead = &block;
}

void OopStorage::RemoveAllocationBlock(Block& block)
{
    if (block.allocationPrev != nullptr) { block.allocationPrev->allocationNext = block.allocationNext; }
    else { allocationHead = block.allocationNext; }
    if (block.allocationNext != nullptr) { block.allocationNext->allocationPrev = block.allocationPrev; }
    block.allocationPrev = block.allocationNext = nullptr;
}

#if defined(MRT_TESTABLE_INTERNALS)
size_t OopStorage::BlockCountForTest() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return activeArray->blocks.size();
}
size_t OopStorage::ConcurrentIterationsForTest() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return concurrentIterationCount;
}
#endif
} // namespace MapleRuntime
