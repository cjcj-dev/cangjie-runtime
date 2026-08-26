// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/RelocationRequestQueue.h"

#include <chrono>
#include <vector>

#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {

void RelocationRequestQueue::BeginWorkers(size_t workers)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    CHECK_DETAIL(!accepting && workers != 0 && workerCount == 0 && synchronizedWorkers == 0 && byFrom.empty(),
                 "invalid relocation worker generation workers=%zu active=%zu synchronized=%zu accepting=%u",
                 workers, workerCount, synchronizedWorkers, static_cast<unsigned>(accepting));
    queue.clear();
    workerCount = workers;
    accepting = true;
}

RelocationRequestQueue::EnqueueResult RelocationRequestQueue::Add(void* owner, MAddress from)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    auto found = byFrom.find(from);
    if (found != byFrom.end()) {
        return EnqueueResult{ found->second, false, true };
    }
    if (!accepting) {
        Handle failed(new Request(owner, from));
        CompleteHandle(failed, 0, State::FAILED);
        return EnqueueResult{ failed, false, false };
    }

    Handle request(new Request(owner, from));
    byFrom.emplace(from, request);
    queue.push_back(request);
    // A worker which observed both queues empty is synchronized on this
    // condition. Notify on the empty->non-empty edge, as in ZRelocateQueue.
    if (queue.size() == 1) {
        queueAttention.notify_all();
    }
    return EnqueueResult{ request, true, true };
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
        const State state = request->requestState.load(std::memory_order_acquire);
        return state == State::COMPLETED || state == State::FAILED;
    });
    const MAddress receipt = request->publishedReceipt.load(std::memory_order_acquire);
    lock.unlock();
    if (stateChanged) {
        (void)mutator->LeaveSaferegion();
    }
    return receipt;
}

void RelocationRequestQueue::WaitUntil(const Handle& request, const std::function<bool()>& pageDone)
{
    if (request == nullptr || pageDone()) {
        return;
    }

    // Keep the mutator in a saferegion exactly as Wait() does, but use the page
    // predicate from ZRelocateQueue::add_and_wait (zRelocate.cpp:134-150).
    // Object completion notifications accelerate the next predicate check;
    // the timed check is the independent exit when this object has no receipt.
    Mutator* mutator = ThreadLocal::GetMutator();
    const ThreadType threadType = ThreadLocal::GetThreadType();
    const bool stateChanged = mutator != nullptr && threadType != ThreadType::FP_THREAD &&
                              threadType != ThreadType::GC_THREAD && mutator->EnterSaferegion(true);
    std::unique_lock<std::mutex> lock(request->completionMutex);
    while (!pageDone()) {
        const State state = request->requestState.load(std::memory_order_acquire);
        // A request can be failed without a page publication (for example when
        // its region claim loses the FROM-list race).  The page predicate is
        // still the normal completion signal, but a terminal request state is
        // an independent, bounded exit so the waiter can take its keep-from
        // fallback instead of polling a page that no longer has a publisher.
        if (state == State::COMPLETED || state == State::FAILED) {
            break;
        }
        (void)request->completion.wait_for(lock, std::chrono::milliseconds(1));
    }
    lock.unlock();
    if (stateChanged) {
        (void)mutator->LeaveSaferegion();
    }
}

bool RelocationRequestQueue::Publish(MAddress from, MAddress receipt)
{
    if (receipt == 0) {
        return false;
    }
    return Complete(from, receipt, State::COMPLETED);
}

bool RelocationRequestQueue::Fail(MAddress from)
{
    return Complete(from, 0, State::FAILED);
}

bool RelocationRequestQueue::Complete(MAddress from, MAddress receipt, State terminalState)
{
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

    CompleteHandle(request, receipt, terminalState);
    completionCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void RelocationRequestQueue::CompleteHandle(const Handle& request, MAddress receipt, State terminalState)
{
    {
        std::lock_guard<std::mutex> lock(request->completionMutex);
        request->publishedReceipt.store(receipt, std::memory_order_relaxed);
        request->requestState.store(terminalState, std::memory_order_release);
    }
    request->completion.notify_all();
}

size_t RelocationRequestQueue::CompleteOwner(
    void* owner, const std::function<MAddress(MAddress)>& resolveReceipt)
{
    std::vector<Handle> owned;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        for (auto it = byFrom.begin(); it != byFrom.end();) {
            if (it->second->owner() == owner) {
                owned.push_back(it->second);
                it = byFrom.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Handle& request : owned) {
        const MAddress receipt = resolveReceipt(request->from());
        CompleteHandle(request, receipt, receipt == 0 ? State::FAILED : State::COMPLETED);
    }
    if (!owned.empty()) {
        completionCount.fetch_add(owned.size(), std::memory_order_relaxed);
    }
    return owned.size();
}

RelocationRequestQueue::Handle RelocationRequestQueue::PruneAndClaimLocked()
{
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

RelocationRequestQueue::Handle RelocationRequestQueue::PruneAndClaim()
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return PruneAndClaimLocked();
}

RelocationRequestQueue::Selection RelocationRequestQueue::SynchronizePoll()
{
    std::unique_lock<std::mutex> lock(queueMutex);
    Handle request = PruneAndClaimLocked();
    if (request != nullptr) {
        return Selection{ request, nullptr, false };
    }

    CHECK_DETAIL(workerCount != 0 && synchronizedWorkers < workerCount,
                 "invalid relocation worker synchronization workers=%zu synchronized=%zu",
                 workerCount, synchronizedWorkers);
    ++synchronizedWorkers;
    if (synchronizedWorkers == workerCount) {
        // All registered workers have observed both request and ordinary work
        // empty. Closing under queueMutex makes the decision atomic with Add.
        accepting = false;
        for (auto& entry : byFrom) {
            CompleteHandle(entry.second, 0, State::FAILED);
        }
        if (!byFrom.empty()) {
            completionCount.fetch_add(byFrom.size(), std::memory_order_relaxed);
        }
        byFrom.clear();
        queue.clear();
        workerCount = 0;
        synchronizedWorkers = 0;
        queueAttention.notify_all();
        return Selection{ nullptr, nullptr, true };
    }

    queueAttention.wait(lock, [this]() { return !accepting || !queue.empty(); });
    if (!accepting) {
        return Selection{ nullptr, nullptr, true };
    }
    CHECK_DETAIL(synchronizedWorkers != 0, "relocation worker synchronization underflow");
    --synchronizedWorkers;
    request = PruneAndClaimLocked();
    return Selection{ request, nullptr, false };
}

size_t RelocationRequestQueue::PendingCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return byFrom.size();
}

size_t RelocationRequestQueue::SynchronizedWorkerCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return synchronizedWorkers;
}

} // namespace MapleRuntime
