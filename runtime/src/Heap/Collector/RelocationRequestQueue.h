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
// the QUEUED -> CLAIMED transition; receipt publication alone owns COMPLETED.
class RelocationRequestQueue {
public:
    enum class State : uint8_t { QUEUED, CLAIMED, COMPLETED };

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
    };

    struct Selection {
        Handle request;
        void* ordinary;

        bool is_request() const { return request != nullptr; }
        explicit operator bool() const { return request != nullptr || ordinary != nullptr; }
    };

    EnqueueResult Add(void* owner, MAddress from);
    MAddress Wait(const Handle& request);

    // Only the first publication for this from-address completes the Request.
    // The notification is per Request, not a queue-wide wakeup.
    bool Publish(MAddress from, MAddress receipt);

    Handle PruneAndClaim();

    // This is the worker ordering point corresponding to zRelocate.cpp:1193-1203:
    // a queued request is claimed before the ordinary relocation iterator runs.
    Selection SelectBeforeOrdinary(const std::function<void*()>& claimOrdinary)
    {
        Handle request = PruneAndClaim();
        if (request != nullptr) {
            return Selection{ request, nullptr };
        }
        return Selection{ nullptr, claimOrdinary() };
    }

    size_t PendingCount() const;
    uint64_t CompletionCount() const { return completionCount.load(std::memory_order_relaxed); }

private:
    mutable std::mutex queueMutex;
    std::deque<Handle> queue;
    std::unordered_map<MAddress, Handle> byFrom;
    std::atomic<uint64_t> completionCount{ 0 };
};

} // namespace MapleRuntime

#endif // MRT_RELOCATION_REQUEST_QUEUE_H
