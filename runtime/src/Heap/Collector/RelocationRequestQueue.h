// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_REQUEST_QUEUE_H
#define MRT_RELOCATION_REQUEST_QUEUE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// ZRelocateQueue::add_and_wait/prune_and_claim (zRelocate.cpp:134-191).
// One Request is shared by all waiters for the same from address. The queue owns
// the QUEUED -> CLAIMED transition; the region owner owns the terminal SUCCESS
// or FAILED transition. Workers synchronize here before leaving so Add cannot
// strand a request between the last empty poll and worker termination.
class RelocationRequestQueue {
public:
    enum class State : uint8_t { QUEUED, CLAIMED, COMPLETED, FAILED };

    class Request {
    public:
        ~Request() = default;
        MAddress from() const { return fromAddress; }
        void* owner() const { return ownerAddress; }
        State state() const { return requestState.load(std::memory_order_acquire); }
        MAddress receipt() const { return publishedReceipt.load(std::memory_order_acquire); }

    private:
        friend class RelocationRequestQueue;
        Request(void* owner, MAddress from) : ownerAddress(owner), fromAddress(from) {}

        void* const ownerAddress;
        const MAddress fromAddress;
        std::atomic<State> requestState{ State::QUEUED };
        std::atomic<MAddress> publishedReceipt{ 0 };
        std::mutex completionMutex;
        std::condition_variable completion;
    };

    using Handle = std::shared_ptr<Request>;

    struct EnqueueResult {
        Handle request;
        bool inserted;
        bool accepted;
    };

    struct Selection {
        Handle request;
        void* ordinary;
        bool workersDone{ false };

        bool is_request() const { return request != nullptr; }
        explicit operator bool() const { return request != nullptr || ordinary != nullptr; }
    };

    // ZRelocateQueue::activate(nworkers) (zRelocate.cpp:80-83) makes queue
    // acceptance and worker registration one lifetime transition. There is no
    // preparation-only opener: accepting requests without a registered worker
    // generation would leave a waiter with no completion owner.
    void BeginWorkers(size_t workers);
    EnqueueResult Add(void* owner, MAddress from);
    MAddress Wait(const Handle& request);

    // Return a published object receipt when COMPLETED wins the wait. A zero
    // result means page completion or a proven no-publisher FAILED terminal;
    // the caller must then resolve from the forwarding table after the wait.
    MAddress WaitUntil(const Handle& request, const std::function<bool()>& pageDone);

    // Only the first publication for this from-address completes the Request.
    // The notification is per Request, not a queue-wide wakeup.
    bool Publish(MAddress from, MAddress receipt);
    bool Fail(MAddress from);

    // Complete every request owned by a region after ForwardRegion returns.
    // A zero resolver result is a failed completion: the region was retained
    // this cycle, so waiters may keep the still-live from address.
    size_t CompleteOwner(void* owner, const std::function<MAddress(MAddress)>& resolveReceipt);

    Handle PruneAndClaim();

    // This is the worker ordering point corresponding to zRelocate.cpp:1193-1203:
    // a queued request is claimed before the ordinary relocation iterator runs.
    Selection SelectBeforeOrdinary(const std::function<void*()>& claimOrdinary)
    {
        Handle request = PruneAndClaim();
        if (request != nullptr) {
            return Selection{ request, nullptr, false };
        }
        return Selection{ nullptr, claimOrdinary(), false };
    }

    // Called only after both request and ordinary polls were empty. Idle
    // workers rendezvous here. Add wakes them while any worker remains; the
    // last synchronized worker closes the generation atomically with Add.
    Selection SynchronizePoll();

    size_t PendingCount() const;
    size_t SynchronizedWorkerCount() const;
    uint64_t CompletionCount() const { return completionCount.load(std::memory_order_relaxed); }

private:
    bool Complete(MAddress from, MAddress receipt, State terminalState);
    static void CompleteHandle(const Handle& request, MAddress receipt, State terminalState);
    Handle PruneAndClaimLocked();

    mutable std::mutex queueMutex;
    std::condition_variable queueAttention;
    std::deque<Handle> queue;
    std::unordered_map<MAddress, Handle> byFrom;
    bool accepting{ false };
    size_t workerCount{ 0 };
    size_t synchronizedWorkers{ 0 };
    std::atomic<uint64_t> completionCount{ 0 };
};

} // namespace MapleRuntime

#endif // MRT_RELOCATION_REQUEST_QUEUE_H
