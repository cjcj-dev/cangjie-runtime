// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
// gc/shared/oopStorage.inline.hpp:132-150. Stable native slots live in blocks;
// handles and language scheduling queues contain pointers to those slots.
// The owning registry's lock protects allocation, release and visitation.
class OopStorage {
    static constexpr size_t SLOTS = sizeof(uintptr_t) * CHAR_BIT;
    struct Slot : NativeSlot { Slot() : NativeSlot(zpointer::null) {} };
    struct Block {
        std::array<Slot, SLOTS> data;
        uintptr_t allocatedBitmask = 0;
        size_t activeIndex = 0;
        Block* allocationPrev = nullptr;
        Block* allocationNext = nullptr;
        bool IsFull() const { return allocatedBitmask == ~uintptr_t(0); }
        NativeSlot* Allocate()
        {
            const unsigned index = static_cast<unsigned>(__builtin_ctzl(~allocatedBitmask));
            allocatedBitmask |= uintptr_t(1) << index;
            return &data[index];
        }
    };
public:
    OopStorage() = default;
    OopStorage(const OopStorage&) = delete;
    OopStorage& operator=(const OopStorage&) = delete;

    NativeSlot* Allocate()
    {
        if (allocationHead == nullptr) {
            std::unique_ptr<Block> block(new Block());
            block->activeIndex = activeArray.size();
            activeArray.push_back(std::move(block));
            AddAllocationBlock(*activeArray.back());
        }
        Block& block = *allocationHead;
        NativeSlot* slot = block.Allocate();
        if (block.IsFull()) { RemoveAllocationBlock(block); }
        ++allocationCount;
        return slot;
    }

    void Release(NativeSlot* slot)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
        for (auto& entry : activeArray) {
            Block& block = *entry;
            const uintptr_t start = reinterpret_cast<uintptr_t>(block.data.data());
            if (address < start || address >= start + sizeof(block.data)) { continue; }
            const size_t index = (address - start) / sizeof(Slot);
            const uintptr_t bit = uintptr_t(1) << index;
            CHECK_DETAIL((block.allocatedBitmask & bit) != 0, "release of inactive native slot");
            const bool wasFull = block.IsFull();
            slot->StoreColoured(zpointer::null, std::memory_order_relaxed);
            block.allocatedBitmask &= ~bit;
            --allocationCount;
            if (wasFull) { AddAllocationBlock(block); }
            return;
        }
        CHECK_DETAIL(false, "native slot belongs to another storage");
    }

    size_t OopsDo(const NativeSlotVisitor& visitor)
    {
        size_t visited = 0;
        for (auto& block : activeArray) {
            uintptr_t remaining = block->allocatedBitmask;
            while (remaining != 0) {
                const unsigned index = static_cast<unsigned>(__builtin_ctzl(remaining));
                visitor(block->data[index]);
                remaining &= remaining - 1;
                ++visited;
            }
        }
        return visited;
    }
    size_t AllocationCount() const { return allocationCount; }

private:
    void AddAllocationBlock(Block& block)
    {
        block.allocationPrev = nullptr;
        block.allocationNext = allocationHead;
        if (allocationHead != nullptr) { allocationHead->allocationPrev = &block; }
        allocationHead = &block;
    }
    void RemoveAllocationBlock(Block& block)
    {
        if (block.allocationPrev != nullptr) { block.allocationPrev->allocationNext = block.allocationNext; }
        else { allocationHead = block.allocationNext; }
        if (block.allocationNext != nullptr) { block.allocationNext->allocationPrev = block.allocationPrev; }
        block.allocationPrev = block.allocationNext = nullptr;
    }
    std::vector<std::unique_ptr<Block>> activeArray;
    Block* allocationHead = nullptr;
    size_t allocationCount = 0;
};

// Language finalizer scheduling adapter. This list owns no root slots: its
// iterators expose the storage slots, preserving the published handle identity.
class NativeRootHandles {
    using List = std::list<NativeSlot*>;
    List handles;
public:
    class iterator {
        friend class NativeRootHandles;
        List::iterator value;
        explicit iterator(List::iterator value) : value(value) {}
    public:
        NativeSlot& operator*() const { return **value; }
        NativeSlot* operator->() const { return *value; }
        iterator& operator++() { ++value; return *this; }
        bool operator!=(const iterator& other) const { return value != other.value; }
        bool operator==(const iterator& other) const { return value == other.value; }
    };
    iterator begin() { return iterator(handles.begin()); }
    iterator end() { return iterator(handles.end()); }
    NativeSlot& front() { return *handles.front(); }
    NativeSlot& back() { return *handles.back(); }
    bool empty() const { return handles.empty(); }
    size_t size() const { return handles.size(); }
    void push_back(NativeSlot* slot) { handles.push_back(slot); }
    iterator erase(iterator position) { return iterator(handles.erase(position.value)); }
    void swap(NativeRootHandles& other) { handles.swap(other.handles); }
    void splice(iterator position, NativeRootHandles& other)
    {
        handles.splice(position.value, other.handles);
    }
};
} // namespace MapleRuntime
