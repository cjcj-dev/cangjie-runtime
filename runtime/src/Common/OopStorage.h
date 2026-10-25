// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
// gc/shared/oopStorage.inline.hpp:132-150: stable slots, allocation bitmap,
// active block array and allocation list. Registry locks protect handles only.
class OopStorage {
    friend class OopStorageTest;
    static constexpr size_t SLOTS = sizeof(uintptr_t) * CHAR_BIT;
    struct Slot : NativeSlot { Slot() : NativeSlot(zpointer::null) {} };
    struct Block {
        std::array<Slot, SLOTS> data;
        std::atomic<uintptr_t> allocatedBitmask{0};
        size_t activeIndex = 0;
        Block* allocationPrev = nullptr;
        Block* allocationNext = nullptr;
        bool IsFull() const { return allocatedBitmask.load(std::memory_order_relaxed) == ~uintptr_t(0); }
        NativeSlot* Allocate();
        size_t Iterate(const NativeSlotVisitor& visitor);
    };
    // Infrastructure adapter: no HotSpot allocator/Mutex rank protocol.
    // shared_ptr owns the active array; iteration count protects its blocks.
    struct ActiveArray { std::vector<Block*> blocks; };
public:
    OopStorage();
    ~OopStorage();
    OopStorage(const OopStorage&) = delete;
    OopStorage& operator=(const OopStorage&) = delete;

    NativeSlot* Allocate();
    void Release(NativeSlot* slot);
    size_t OopsDo(const NativeSlotVisitor& visitor);
    size_t AllocationCount() const;

    // oopStorage.hpp:174-190: the owner is notified after weak processing.
    using NumDeadCallback = void (*)(size_t);
    void register_num_dead_callback(NumDeadCallback callback);
    bool should_report_num_dead() const;
    void report_num_dead(size_t numDead) const;

    // oopStorage.cpp:1051-1127 / oopStorageParState.inline.hpp:52-65.
    // This state belongs to the root task and is shared by all its workers.
    class BasicParState {
    public:
        BasicParState(OopStorage& storage, unsigned estimatedThreadCount, bool concurrent);
        ~BasicParState();
        BasicParState(const BasicParState&) = delete;
        BasicParState& operator=(const BasicParState&) = delete;
        size_t Iterate(const NativeSlotVisitor& visitor);
        const OopStorage* Storage() const { return &storage; }
        size_t num_dead() const { return numDead.load(std::memory_order_relaxed); }
        void increment_num_dead(size_t count) { numDead.fetch_add(count, std::memory_order_relaxed); }
    private:
        struct IterationData {
            size_t segmentStart = 0;
            size_t segmentEnd = 0;
            size_t processed = 0;
        };
        bool ClaimNextSegment(IterationData& data);
        OopStorage& storage;
        std::shared_ptr<ActiveArray> activeArray;
        size_t blockCount;
        std::atomic<size_t> nextBlock{0};
        std::atomic<size_t> numDead{0};
        const unsigned estimatedThreadCount;
        const bool concurrent;
    };
    template<bool concurrent>
    class ParState {
    public:
        ParState(OopStorage& storage, unsigned estimatedThreadCount = 1)
            : basicState(storage, estimatedThreadCount, concurrent) {}
        size_t OopsDo(const NativeSlotVisitor& visitor) { return basicState.Iterate(visitor); }
        const OopStorage* storage() const { return basicState.Storage(); }
        size_t num_dead() const { return basicState.num_dead(); }
        void increment_num_dead(size_t count) { basicState.increment_num_dead(count); }
    private:
        BasicParState basicState;
    };
private:
    void AddAllocationBlock(Block& block);
    void RemoveAllocationBlock(Block& block);
    void EnsureWritableArray();
    void DeleteEmptyBlocks(); // called with mutex held; deferred during iteration
    mutable std::mutex mutex;
    std::shared_ptr<ActiveArray> activeArray;
    size_t concurrentIterationCount = 0;
    Block* allocationHead = nullptr;
    size_t allocationCount = 0;
    NumDeadCallback numDeadCallback = nullptr;
};

} // namespace MapleRuntime
