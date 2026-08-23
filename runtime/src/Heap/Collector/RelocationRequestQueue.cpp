// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/RelocationRequestQueue.h"

#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {

RelocationRequestQueue::EnqueueResult RelocationRequestQueue::Add(void* owner, MAddress from)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    auto found = byFrom.find(from);
    if (found != byFrom.end()) {
        return EnqueueResult{ found->second, false };
    }

    Handle request(new Request(owner, from));
    byFrom.emplace(from, request);
    queue.push_back(request);
    return EnqueueResult{ request, true };
}

MAddress RelocationRequestQueue::Wait(const Handle& request)
{
    if (request == nullptr) {
        return 0;
    }

    // A relocation requester is normally a bound mutator. Consult the runtime
    // TLS directly: falling back through ConcurrencyModel here would call into
    // the scheduler even for an unattached native waiter.
    Mutator* mutator = ThreadLocal::GetMutator();
    const ThreadType threadType = ThreadLocal::GetThreadType();
    const bool stateChanged = mutator != nullptr && threadType != ThreadType::FP_THREAD &&
                              threadType != ThreadType::GC_THREAD && mutator->EnterSaferegion(true);
    std::unique_lock<std::mutex> lock(request->completionMutex);
    request->completion.wait(lock, [&request]() {
        return request->requestState.load(std::memory_order_acquire) == State::COMPLETED;
    });
    const MAddress receipt = request->publishedReceipt.load(std::memory_order_acquire);
    lock.unlock();
    if (stateChanged) {
        (void)mutator->LeaveSaferegion();
    }
    return receipt;
}

bool RelocationRequestQueue::Publish(MAddress from, MAddress receipt)
{
    if (receipt == 0) {
        return false;
    }

    Handle request;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto found = byFrom.find(from);
        if (found == byFrom.end()) {
            return false;
        }
        request = found->second;
        byFrom.erase(found);
    }

    {
        std::lock_guard<std::mutex> lock(request->completionMutex);
        request->publishedReceipt.store(receipt, std::memory_order_relaxed);
        request->requestState.store(State::COMPLETED, std::memory_order_release);
    }
    completionCount.fetch_add(1, std::memory_order_relaxed);
    request->completion.notify_all();
    return true;
}

RelocationRequestQueue::Handle RelocationRequestQueue::PruneAndClaim()
{
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!queue.empty()) {
        Handle request = queue.front();
        queue.pop_front();
        State expected = State::QUEUED;
        if (request->requestState.compare_exchange_strong(expected, State::CLAIMED, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            return request;
        }
    }
    return nullptr;
}

size_t RelocationRequestQueue::PendingCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return byFrom.size();
}

} // namespace MapleRuntime
