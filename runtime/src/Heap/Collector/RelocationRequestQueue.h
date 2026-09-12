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
#include "Heap/Allocator/ForwardingTable.h"

namespace MapleRuntime {

// ZRelocateQueue::add_and_wait/prune_and_claim (zRelocate.cpp:134-191).
// One handle per forwarding, shared by every object on the page. Claim and
// completion live in ZForwarding; the queue stores neither an object address
// nor an independent answer. Worker rendezvous is atomic with enqueue.
class RelocationRequestQueue {
public:
    enum class State : uint8_t { QUEUED, CLAIMED, COMPLETED };

    class Request {
    public:
        MAddress from() const { return forwarding ? forwarding->start() : 0; }
        void* owner() const { return forwarding ? forwarding->page() : nullptr; }
        ZForwarding* page_forwarding() const { return forwarding.get(); }
        State state() const
        {
            if (forwarding->is_done()) return State::COMPLETED;
            return forwarding->claimed().load(std::memory_order_acquire) ? State::CLAIMED : State::QUEUED;
        }
    private:
        friend class RelocationRequestQueue;
        explicit Request(ForwardingTable::Owner value) : forwarding(std::move(value)) {}
        ForwardingTable::Owner forwarding;
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
    EnqueueResult Add(ForwardingTable::Owner forwarding);
    MAddress Wait(const Handle& request);

    // Wait for the canonical forwarding completion. Always returns zero;
    // callers resolve their own object through the forwarding table afterward.
    MAddress WaitUntil(const Handle& request, size_t maxSpins = 0, bool* timedOut = nullptr);

    size_t Complete(ZForwarding* forwarding);

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

    bool IsActive() const;
    size_t PendingCount() const;
    size_t SynchronizedWorkerCount() const;
    uint64_t CompletionCount() const { return completionCount.load(std::memory_order_relaxed); }

#if defined(MRT_TESTABLE_INTERNALS)
    using WaitEnterHook = void (*)(ZForwarding* forwarding);
    static void SetWaitEnterHook(WaitEnterHook hook);
#endif

private:
    Handle PruneAndClaimLocked();
    void PruneDoneLocked();

    mutable std::mutex queueMutex;
    std::condition_variable queueAttention;
    std::deque<Handle> queue;
    std::unordered_map<ZForwarding*, Handle> byPage;
    bool accepting{ false };
    size_t workerCount{ 0 };
    size_t synchronizedWorkers{ 0 };
    std::atomic<uint64_t> completionCount{ 0 };
};

} // namespace MapleRuntime

#endif // MRT_RELOCATION_REQUEST_QUEUE_H
