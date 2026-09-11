// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/RelocationRequestQueue.h"

#include <chrono>
#include "Heap/Allocator/RegionInfo.h"
#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {

void RelocationRequestQueue::BeginWorkers(size_t workers)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    PruneDoneLocked();
    CHECK_DETAIL(!accepting && workers != 0 && workerCount == 0 && synchronizedWorkers == 0 && byPage.empty(),
                 "invalid relocation worker generation workers=%zu active=%zu synchronized=%zu accepting=%u",
                 workers, workerCount, synchronizedWorkers, static_cast<unsigned>(accepting));
    queue.clear();
    workerCount = workers;
    accepting = true;
}

RelocationRequestQueue::EnqueueResult RelocationRequestQueue::Add(void* region, MAddress from)
{
    auto owner = ForwardingTable::RetainPageOwner(static_cast<RegionInfo*>(region));
    CHECK_DETAIL(!owner || owner->covers(from), "relocation request outside forwarding from=%#zx", from);
    return Add(std::move(owner));
}

RelocationRequestQueue::EnqueueResult RelocationRequestQueue::Add(ForwardingTable::Owner forwarding)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!forwarding) return { nullptr, false, false };
    auto found = byPage.find(forwarding.get());
    if (found != byPage.end()) return { found->second, false, true };
    if (forwarding->is_done()) return { Handle(new Request(std::move(forwarding))), false, true };
    // An already claimed forwarding has its own completion owner even after
    // the queue's last worker left. An unclaimed page requires an active task.
    if (!accepting && !forwarding->claimed().load(std::memory_order_acquire)) {
        return { nullptr, false, false };
    }
    Handle request(new Request(std::move(forwarding)));
    byPage.emplace(request->page_forwarding(), request);
    queue.push_back(request);
    queueAttention.notify_all();
    return { request, true, true };
}

MAddress RelocationRequestQueue::Wait(const Handle& request)
{
    return WaitUntil(request);
}

MAddress RelocationRequestQueue::WaitUntil(const Handle& request, size_t maxSpins, bool* timedOut)
{
    if (timedOut != nullptr) *timedOut = false;
    if (request == nullptr) return 0;
    Mutator* mutator = ThreadLocal::GetMutator();
    const ThreadType type = ThreadLocal::GetThreadType();
    const bool changed = mutator != nullptr && type != ThreadType::FP_THREAD && type != ThreadType::GC_THREAD &&
                         mutator->EnterSaferegion(true);
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        size_t spins = 0;
        while (!request->page_forwarding()->is_done()) {
            if (maxSpins != 0 && spins >= maxSpins) {
                if (timedOut != nullptr) *timedOut = true;
                break;
            }
            // mark_done may be published by an already-running page claimant.
            // The predicate is the forwarding's immutable completion, never a
            // RegionInfo incarnation or per-object publication.
            queueAttention.wait_for(lock, std::chrono::milliseconds(1));
            ++spins;
        }
    }
    if (changed) (void)mutator->LeaveSaferegion();
    return 0;
}

size_t RelocationRequestQueue::Complete(ZForwarding* forwarding)
{
    std::lock_guard<std::mutex> lock(queueMutex);
    const size_t completed = forwarding != nullptr && forwarding->is_done() && byPage.count(forwarding) != 0 ? 1 : 0;
    PruneDoneLocked();
    queueAttention.notify_all();
    return completed;
}

void RelocationRequestQueue::PruneDoneLocked()
{
    for (auto it = queue.begin(); it != queue.end();) {
        if ((*it)->page_forwarding()->is_done()) {
            byPage.erase((*it)->page_forwarding());
            it = queue.erase(it);
            completionCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
}

RelocationRequestQueue::Handle RelocationRequestQueue::PruneAndClaimLocked()
{
    PruneDoneLocked();
    for (const auto& request : queue) {
        if (request->page_forwarding()->claim()) return request;
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
    if (request) return { request, nullptr, false };
    CHECK_DETAIL(workerCount != 0 && synchronizedWorkers < workerCount,
                 "invalid relocation worker synchronization workers=%zu synchronized=%zu",
                 workerCount, synchronizedWorkers);
    ++synchronizedWorkers;
    if (synchronizedWorkers == workerCount) {
        // All real page tasks have returned before joining this rendezvous.
        // A claimed external owner can finish independently; do not relabel it.
        accepting = false;
        workerCount = 0;
        synchronizedWorkers = 0;
        queueAttention.notify_all();
        return { nullptr, nullptr, true };
    }
    for (;;) {
        queueAttention.wait(lock);
        if (!accepting) return { nullptr, nullptr, true };
        request = PruneAndClaimLocked();
        if (request) {
            --synchronizedWorkers;
            return { request, nullptr, false };
        }
    }
}

bool RelocationRequestQueue::IsActive() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return accepting;
}

size_t RelocationRequestQueue::PendingCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return byPage.size();
}

size_t RelocationRequestQueue::SynchronizedWorkerCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return synchronizedWorkers;
}

} // namespace MapleRuntime
