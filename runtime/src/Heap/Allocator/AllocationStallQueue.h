// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ALLOCATION_STALL_QUEUE_H
#define MRT_ALLOCATION_STALL_QUEUE_H

#include <condition_variable>
#include <cstddef>
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

    bool Wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
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
    const size_t size;
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
        const bool requestGc = requests.empty();
        requests.push_back(&request);
        ++enqueued;
#if defined(MRT_ALLOCATION_STALL_CUT_ENQUEUE)
        (void)requestGc;
        return false;
#else
        return requestGc;
#endif
    }

    size_t SatisfyAvailable(const std::function<bool(size_t)>& available)
    {
#if defined(MRT_ALLOCATION_STALL_CUT_SATISFY)
        (void)available;
        return 0;
#else
        size_t satisfied = 0;
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.empty()) {
            AllocationStallRequest* request = requests.front();
            if (!available(request->GetSize())) {
                break;
            }
            requests.pop_front();
            request->Satisfy(true);
            ++satisfied;
#if !defined(MRT_ALLOCATION_STALL_CUT_DEQUEUE)
            ++dequeued;
#endif
            ++satisfiedCount;
        }
        return satisfied;
#endif
    }

    size_t FailAll()
    {
        size_t failed = 0;
        std::lock_guard<std::mutex> lock(mutex);
        while (!requests.empty()) {
            AllocationStallRequest* request = requests.front();
            requests.pop_front();
            request->Satisfy(false);
            ++failed;
            ++dequeued;
            ++failedCount;
        }
        return failed;
    }

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

private:
    mutable std::mutex mutex;
    std::deque<AllocationStallRequest*> requests;
    size_t enqueued{ 0 };
    size_t dequeued{ 0 };
    size_t satisfiedCount{ 0 };
    size_t failedCount{ 0 };
};

} // namespace MapleRuntime

#endif // MRT_ALLOCATION_STALL_QUEUE_H
