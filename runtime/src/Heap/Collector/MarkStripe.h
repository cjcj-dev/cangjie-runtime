// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_STRIPE_H
#define MRT_MARK_STRIPE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Heap/Collector/MarkStackEntry.h"

namespace MapleRuntime {

class RegionInfo;

// ZGC zMarkStack.hpp:35-54. A chunk is single-owner while it is being
// filled/drained and becomes immutable while linked on a shared stripe.
class MarkStripeStack {
public:
    static MarkStripeStack* Create(bool firstStack);
    static void Destroy(MarkStripeStack* stack);

    explicit MarkStripeStack(size_t capacity);
    MarkStripeStack(const MarkStripeStack&) = delete;
    MarkStripeStack& operator=(const MarkStripeStack&) = delete;

    bool IsEmpty() const { return top == 0; }
    bool IsFull() const { return top == capacity; }
    size_t Size() const { return top; }
    size_t Capacity() const { return capacity; }
    void Push(const MarkStackEntry& entry);
    MarkStackEntry Pop();

private:
    size_t top = 0;
    size_t capacity;
    std::unique_ptr<MarkStackEntry[]> entries;
};

class MarkStripeStackListNode {
public:
    explicit MarkStripeStackListNode(MarkStripeStack* stack) : stack(stack) {}

    MarkStripeStack* Stack() const { return stack; }
    MarkStripeStackListNode* Next() const { return next; }
    void SetNext(MarkStripeStackListNode* value) { next = value; }

private:
    MarkStripeStack* const stack;
    MarkStripeStackListNode* next = nullptr;
};

// Hazard-pointer safe memory reclamation for the lock-free stack nodes. Each
// worker owns exactly one WorkerState; only hazard scans read remote states.
// Ported from ZGC zMarkingSMR.cpp:34-112.
class MarkingSMR {
public:
    explicit MarkingSMR(size_t workerCount);
    ~MarkingSMR();
    MarkingSMR(const MarkingSMR&) = delete;
    MarkingSMR& operator=(const MarkingSMR&) = delete;

    size_t WorkerCount() const { return workerCount; }
    std::atomic<MarkStripeStackListNode*>& Hazard(size_t workerId);
    void Retire(size_t workerId, MarkStripeStackListNode* node);
    void Reclaim(size_t workerId);
    void Free();

    // White-box evidence for the ABA positive/safe control. Production pop
    // uses the same hazard slots; these accessors do not alter reclamation.
    size_t PendingCount(size_t workerId) const;

private:
    struct alignas(64) WorkerState {
        std::atomic<MarkStripeStackListNode*> hazard{ nullptr };
        std::vector<MarkStripeStackListNode*> scannedHazards;
        std::vector<MarkStripeStackListNode*> freeing;
    };

    size_t workerCount;
    std::unique_ptr<WorkerState[]> workers;
};

class alignas(64) MarkStripeStackList {
public:
    MarkStripeStackList() = default;
    MarkStripeStackList(const MarkStripeStackList&) = delete;
    MarkStripeStackList& operator=(const MarkStripeStackList&) = delete;

    bool IsEmpty() const { return head.load(std::memory_order_acquire) == nullptr; }
    size_t Length() const;
    void Push(MarkStripeStack* stack);
    MarkStripeStack* Pop(MarkingSMR& smr, size_t workerId);

private:
    std::atomic<MarkStripeStackListNode*> head{ nullptr };
    std::atomic<ptrdiff_t> length{ 0 };
};

class MarkStripe {
public:
    bool IsEmpty() const { return published.IsEmpty() && overflowed.IsEmpty(); }
    size_t Population() const { return published.Length() + overflowed.Length(); }
    void PublishStack(MarkStripeStack* stack, bool publish);
    MarkStripeStack* StealStack(MarkingSMR& smr, size_t workerId);

private:
    // Keep producer-published and worker-overflow stacks separate, like ZGC,
    // so GC workers do not contend on the mutator/root publication head.
    MarkStripeStackList published;
    MarkStripeStackList overflowed;
};

class MarkStripeSet {
public:
    explicit MarkStripeSet(size_t stripeCount);
    MarkStripeSet(const MarkStripeSet&) = delete;
    MarkStripeSet& operator=(const MarkStripeSet&) = delete;

    size_t Count() const { return stripes.size(); }
    bool IsEmpty() const;
    size_t StripeForAddress(uintptr_t address) const;
    size_t StripeForWorker(size_t workerCount, size_t workerId) const;
    size_t Next(size_t stripeId) const { return (stripeId + 1) & mask; }
    MarkStripe& At(size_t stripeId) { return *stripes[stripeId]; }
    const MarkStripe& At(size_t stripeId) const { return *stripes[stripeId]; }

private:
    size_t mask;
    std::vector<std::unique_ptr<MarkStripe>> stripes;
};

class MarkThreadLocalStacks {
public:
    explicit MarkThreadLocalStacks(size_t stripeCount);
    ~MarkThreadLocalStacks();
    MarkThreadLocalStacks(const MarkThreadLocalStacks&) = delete;
    MarkThreadLocalStacks& operator=(const MarkThreadLocalStacks&) = delete;

    bool IsEmpty() const;
    void Push(MarkStripeSet& stripes, size_t stripeId, const MarkStackEntry& entry, bool publish);
    bool Pop(MarkingSMR& smr, size_t workerId, MarkStripeSet& stripes, size_t stripeId,
             MarkStackEntry& entry);
    MarkStripeStack* StealLocal(size_t stripeId);
    void Install(size_t stripeId, MarkStripeStack* stack);
    bool Flush(MarkStripeSet& stripes, bool publish);

private:
    std::vector<MarkStripeStack*> stacks;
};

// ZGC ZMarkCache analogue. Mark-bit claims remain atomic; only the page/region
// live-byte additions are coalesced per worker.
class MarkLiveCache {
public:
    explicit MarkLiveCache(size_t stripeCount);
    ~MarkLiveCache();
    MarkLiveCache(const MarkLiveCache&) = delete;
    MarkLiveCache& operator=(const MarkLiveCache&) = delete;

    void IncLive(RegionInfo* region, size_t bytes);
    void Flush();

private:
    struct Entry {
        RegionInfo* region = nullptr;
        size_t bytes = 0;
    };

    void Evict(Entry& entry);
    size_t shift;
    static constexpr size_t CACHE_SIZE = 64;
    Entry entries[CACHE_SIZE];
};

// Per-worker follow-work context: natural stripe + private stacks + live cache.
class MarkContext {
public:
    MarkContext(size_t workerCount, size_t workerId, MarkStripeSet& stripes);

    size_t StripeId() const { return stripeId; }
    void SetStripeId(size_t value) { stripeId = value; }
    MarkThreadLocalStacks& Stacks() { return stacks; }
    MarkLiveCache& Cache() { return cache; }

private:
    size_t stripeId;
    MarkThreadLocalStacks stacks;
    MarkLiveCache cache;
};

} // namespace MapleRuntime

#endif // MRT_MARK_STRIPE_H
