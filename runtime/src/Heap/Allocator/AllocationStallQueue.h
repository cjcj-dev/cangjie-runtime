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

namespace MapleRuntime {

// One object represents one blocked allocation.  It is deliberately owned by
// the allocator caller; the queue only retains the pointer until a terminal
// answer is published.
class AllocationStallRequest {
public:
    explicit AllocationStallRequest(size_t size) : size(size) {}
    AllocationStallRequest(const AllocationStallRequest&) = delete;
    AllocationStallRequest& operator=(const AllocationStallRequest&) = delete;

    size_t GetSize() const { return size; }
    size_t GetClaimedUnits() const { return claimedUnits; }

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
    size_t claimedUnits{ 0 };
    std::mutex mutex;
    std::condition_variable condition;
    bool completed{ false };
    bool result{ false };
};

// Allocator-owned FIFO.  Enqueue returns true only for the transition from
// empty to non-empty, giving the first waiter ownership of the GC request.
class AllocationStallQueue {
public:
    bool Enqueue(AllocationStallRequest& request)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const bool requestGc = !gcInProgress;
        gcInProgress = true;
        request.sequence = ++lastSequence;
        requests.push_back(&request);
#if defined(MRT_GC_UNIT_TESTS)
        ++enqueued;
#endif
#if defined(MRT_ALLOCATION_STALL_CUT_ENQUEUE)
        (void)requestGc;
        return false;
#else
        return requestGc;
#endif
    }

    uint64_t CaptureWaveBoundary() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return lastSequence;
    }

    size_t SatisfyAvailable(const std::function<size_t(size_t, size_t)>& claim)
    {
#if defined(MRT_ALLOCATION_STALL_CUT_SATISFY)
        (void)claim;
        return 0;
#else
        size_t satisfied = 0;
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.empty()) {
            AllocationStallRequest* request = requests.front();
            const size_t units = claim(request->GetSize(), claimedUnits);
            if (units == 0) {
                break;
            }
#if defined(MRT_ALLOCATION_STALL_CUT_DEQUEUE)
            request->claimedUnits = units;
            claimedUnits += units;
            request->Satisfy(true);
#else
            requests.pop_front();
            request->claimedUnits = units;
            claimedUnits += units;
            request->Satisfy(true);
#endif
            ++satisfied;
#if defined(MRT_GC_UNIT_TESTS)
            ++dequeued;
            ++satisfiedCount;
#endif
        }
        return satisfied;
#endif
    }

    bool CompleteWave(uint64_t boundary)
    {
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.empty() && requests.front()->sequence <= boundary) {
            AllocationStallRequest* request = requests.front();
            requests.pop_front();
            request->Satisfy(false);
#if defined(MRT_GC_UNIT_TESTS)
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

    void ReleaseClaim(size_t units)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (units <= claimedUnits) {
            claimedUnits -= units;
        }
    }

#if defined(MRT_GC_UNIT_TESTS)
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
    mutable std::mutex mutex;
    std::deque<AllocationStallRequest*> requests;
    uint64_t lastSequence{ 0 };
    size_t claimedUnits{ 0 };
    bool gcInProgress{ false };
#if defined(MRT_GC_UNIT_TESTS)
    size_t enqueued{ 0 };
    size_t dequeued{ 0 };
    size_t satisfiedCount{ 0 };
    size_t failedCount{ 0 };
#endif
};

} // namespace MapleRuntime

#endif // MRT_ALLOCATION_STALL_QUEUE_H
