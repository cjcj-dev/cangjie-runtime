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
#include "Base/Macros.h"

#include "Heap/z/zMarkStackEntry.hpp"
#include "Heap/z/zAttachedArray.hpp"

#include "Heap/z/zMarkingSMR.hpp"

#include "Heap/z/zMarkCache.hpp"

#include "Heap/z/zMarkContext.hpp"
#include "Heap/z/zPageFwd.hpp"

namespace MapleRuntime {

class BaseObject;

// ZGC zMarkStack.hpp:35-54. A chunk is single-owner while it is being
// filled/drained and becomes immutable while linked on a shared stripe.
class MarkStripeStack {
public:
    static MarkStripeStack* Create(bool firstStack);
    static void Destroy(MarkStripeStack* stack);

    MarkStripeStack(const MarkStripeStack&) = delete;
    MarkStripeStack& operator=(const MarkStripeStack&) = delete;

    bool IsEmpty() const;
    bool IsFull() const;
    size_t Size() const;
    size_t Capacity() const;
    void Push(const MarkStackEntry& entry);
    MarkStackEntry Pop();


private:
    using AttachedArray = ZAttachedArray<MarkStripeStack, MarkStackEntry>;
    explicit MarkStripeStack(size_t capacity);
    ~MarkStripeStack() = default;
    size_t top = 0;
    AttachedArray entries;
};

class MarkStripeStackListNode {
public:
    explicit MarkStripeStackListNode(MarkStripeStack* stack);

    MarkStripeStack* Stack() const;
    MarkStripeStackListNode* Next() const;
    void SetNext(MarkStripeStackListNode* value);

private:
    MarkStripeStack* const stack;
    MarkStripeStackListNode* next = nullptr;
};

// Hazard-pointer safe memory reclamation for the lock-free stack nodes. Each
// worker owns exactly one WorkerState; only hazard scans read remote states.
// Ported from ZGC zMarkingSMR.cpp:34-112.


class alignas(64) MarkStripeStackList {
public:
    MarkStripeStackList() = default;
    MarkStripeStackList(const MarkStripeStackList&) = delete;
    MarkStripeStackList& operator=(const MarkStripeStackList&) = delete;

    bool IsEmpty() const;
    size_t Length() const;
    void Push(MarkStripeStack* stack);
    MarkStripeStack* Pop(MarkingSMR& smr, size_t workerId);

private:
    std::atomic<MarkStripeStackListNode*> head{ nullptr };
    std::atomic<ptrdiff_t> length{ 0 };
};

class MarkTerminate;

class MarkStripe {
public:
    bool IsEmpty() const;
    size_t Population() const;
    void PublishStack(MarkStripeStack* stack, bool publish, MarkTerminate* terminate = nullptr);
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
    size_t NStripes() const;
    size_t NStripesMask() const { return nstripesMask.load(std::memory_order_relaxed); }
    void SetNStripes(size_t value);
    bool TrySetNStripes(size_t oldNStripes, size_t newNStripes);
    size_t CalculateNStripes(size_t nworkers) const;
    bool IsCrowded() const;
    void SetTerminate(MarkTerminate* value) { terminate = value; }
    MarkTerminate* Terminate() const { return terminate; }
    bool IsEmpty() const;
    size_t Population() const;
    size_t FirstNonEmptyStripe() const;
    size_t StripeForAddress(uintptr_t address) const;
    size_t StripeForWorker(size_t workerCount, size_t workerId) const;
    size_t Next(size_t stripeId) const;
    size_t Next(size_t stripeId, size_t offset) const;
    MarkStripe& At(size_t stripeId);
    const MarkStripe& At(size_t stripeId) const;

private:
    size_t capacityMask;
    std::atomic<size_t> nstripesMask;
    MarkTerminate* terminate = nullptr;
    std::vector<std::unique_ptr<MarkStripe>> stripes;
};

class MarkThreadLocalStacks {
public:
    MarkThreadLocalStacks() : MarkThreadLocalStacks(16) {}
    explicit MarkThreadLocalStacks(size_t stripeCount);
    ~MarkThreadLocalStacks();
    MarkThreadLocalStacks(const MarkThreadLocalStacks&) = delete;
    MarkThreadLocalStacks& operator=(const MarkThreadLocalStacks&) = delete;

    bool IsEmpty() const;
    size_t Population() const;
    void Push(MarkStripeSet& stripes, size_t stripeId, const MarkStackEntry& entry, bool publish);
    bool Pop(MarkingSMR& smr, size_t workerId, MarkStripeSet& stripes, size_t stripeId,
             MarkStackEntry& entry);
    MarkStripeStack* StealLocal(size_t stripeId);
    void Install(size_t stripeId, MarkStripeStack* stack);
    bool Flush(MarkStripeSet& stripes);

private:
    std::vector<MarkStripeStack*> stacks;
};

// ZGC ZMarkCache analogue. Mark-bit claims remain atomic; only the page/region
// live-object and aligned-byte additions are coalesced per worker.

// Per-worker follow-work context: natural stripe + private stacks + live cache.


} // namespace MapleRuntime

#endif // MRT_MARK_STRIPE_H
