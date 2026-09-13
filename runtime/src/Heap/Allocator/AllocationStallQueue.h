// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ALLOCATION_STALL_QUEUE_H
#define MRT_ALLOCATION_STALL_QUEUE_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
#include "MemMap.h"

#if defined(MRT_GC_UNIT_TESTS) || defined(MRT_TESTABLE_INTERNALS)
#define MRT_ALLOCATION_STALL_OBSERVE 1
#endif

namespace MapleRuntime {

// ZVirtualMemory represented in heap granules; ownership travels with the
// page allocation until materialization or hand-back. A02c supplies cache
// partition selection; the current allocator has one logical partition.
struct PageMemory {
    size_t index{ 0 };
    size_t units{ 0 };
    uint32_t partition{ 0 };
    bool committed{ false };
    // ZMemoryAllocation::partial_vmems: these extents leave the mapped cache
    // under the allocator owner and travel with the allocation request.
    std::vector<MemoryRange> partialMappings;
    bool virtualClaimed{ true };
    size_t harvestedUnits{ 0 };

};

// One object represents one blocked allocation.  It is deliberately owned by
// the allocator caller; the queue only retains the pointer until a terminal
// answer is published.
class AllocationStallRequest {
public:
    AllocationStallRequest(size_t size, uint8_t role, bool physical, bool clear)
        : size(size), role(role), physical(physical), clear(clear) {}
    AllocationStallRequest(const AllocationStallRequest&) = delete;
    AllocationStallRequest& operator=(const AllocationStallRequest&) = delete;

    size_t GetSize() const { return size; }
    uint8_t GetRole() const { return role; }
    bool ExpectsPhysicalMemory() const { return physical; }
    bool ClearsPayload() const { return clear; }
    PageMemory& Memory() { return memory; }
    const PageMemory& Memory() const { return memory; }

    bool Wait(const std::function<void()>& beforeWait = {})
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!completed && beforeWait) {
            beforeWait();
        }
        condition.wait(lock, [this] { return completed; });
        return result;
    }

    void Satisfy(bool value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return;
            }
            result = value;
            completed = true;
        }
        condition.notify_one();
    }

private:
    friend class AllocationStallQueue;

    const size_t size;
    uint64_t sequence{ 0 };
    const uint8_t role;
    const bool physical;
    const bool clear;
    PageMemory memory;
    std::mutex mutex;
    std::condition_variable condition;
    bool completed{ false };
    bool result{ false };
};

// Allocator-owned FIFO.  Enqueue returns true only for the transition from
// empty to non-empty, giving the first waiter ownership of the GC request.
class AllocationStallQueue {
public:
    explicit AllocationStallQueue(std::mutex& owner) : mutex(owner) {}

    // The allocator holds the same owner across claim failure and enqueue.
    bool EnqueueLocked(AllocationStallRequest& request)
    {
        const bool requestGc = !gcInProgress;
        gcInProgress = true;
        request.sequence = ++lastSequence;
        requests.push_back(&request);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
        ++enqueued;
#endif
        return requestGc;
    }

    uint64_t CaptureWaveBoundary() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return lastSequence;
    }

    size_t SatisfyAvailable(const std::function<bool(AllocationStallRequest&)>& claim)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return SatisfyAvailableLocked(claim);
    }

    size_t SatisfyAvailableLocked(const std::function<bool(AllocationStallRequest&)>& claim)
    {
        size_t satisfied = 0;
        while (!requests.empty()) {
            AllocationStallRequest* request = requests.front();
            if (!claim(*request)) {
                break;
            }
            requests.pop_front();
            request->Satisfy(true);
            ++satisfied;
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            ++dequeued;
            ++satisfiedCount;
#endif
        }
        return satisfied;
    }

    bool CompleteWave(uint64_t boundary)
    {
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.empty() && requests.front()->sequence <= boundary) {
            AllocationStallRequest* request = requests.front();
            requests.pop_front();
            request->Satisfy(false);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            ++dequeued;
            ++failedCount;
#endif
        }
        if (requests.empty()) {
            gcInProgress = false;
            return false;
        }
        return true;
    }

#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    size_t Pending() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return requests.size();
    }
    size_t EnqueuedCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return enqueued;
    }
    size_t DequeuedCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return dequeued;
    }
    size_t SatisfiedCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return satisfiedCount;
    }
    size_t FailedCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return failedCount;
    }
#endif

private:
    std::mutex& mutex;
    std::deque<AllocationStallRequest*> requests;
    uint64_t lastSequence{ 0 };
    bool gcInProgress{ false };
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    size_t enqueued{ 0 };
    size_t dequeued{ 0 };
    size_t satisfiedCount{ 0 };
    size_t failedCount{ 0 };
#endif
};

} // namespace MapleRuntime

#endif // MRT_ALLOCATION_STALL_QUEUE_H
