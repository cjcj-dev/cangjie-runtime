#include "Base/Globals.h"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <limits>

#include <algorithm>
#include <atomic>
#include <new>
#include <type_traits>
#include <vector>

#include "Base/Log.h"
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
namespace {
constexpr size_t FIRST_STACK_CAPACITY = 128;
constexpr size_t REGULAR_STACK_CAPACITY = 512;
} // namespace


MarkStripeStack* MarkStripeStack::Create(bool firstStack)
{
    // ZGC zMarkStack.cpp:33-47: one allocation owns the header and entries.
    // Only these bounded capacities reach ZAttachedArray's size arithmetic.
    static_assert(std::is_trivially_destructible<MarkStackEntry>::value,
                  "attached entries must not require per-element destruction");
    const size_t capacity = firstStack ? FIRST_STACK_CAPACITY : REGULAR_STACK_CAPACITY;
    void* const memory = AttachedArray::alloc(capacity);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* const stack = ::new (memory) MarkStripeStack(capacity);
    return stack;
}

void MarkStripeStack::Destroy(MarkStripeStack* stack)
{
    // Local slots can be null, unlike the non-null-only ZGC destroy caller.
    if (stack == nullptr) {
        return;
    }
    stack->~MarkStripeStack();
    AttachedArray::free(stack);
}

MarkStripeStack::MarkStripeStack(size_t capacity)
    : entries(capacity)
{}

size_t MarkStripeStackList::Length() const
{
    const ptrdiff_t value = length.load(std::memory_order_relaxed);
    return value < 0 ? 0 : static_cast<size_t>(value);
}

void MarkStripeStackList::Push(MarkStripeStack* stack)
{
    CHECK_DETAIL(stack != nullptr && !stack->IsEmpty(), "never publish an empty mark stripe stack");
    auto* const node = new (std::nothrow) MarkStripeStackListNode(stack);
    CHECK_DETAIL(node != nullptr, "failed to allocate mark stripe list node");

    MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
    for (;;) {
        node->SetNext(observed);
        // ABA on push is benign: the observed node is never dereferenced.
        if (head.compare_exchange_weak(observed, node, std::memory_order_release, std::memory_order_relaxed)) {
            length.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

MarkStripeStack* MarkStripeStackList::Pop(MarkingSMR& smr, size_t /* workerId */)
{
    std::atomic<MarkStripeStackListNode*>& hazard = *smr.hazard_ptr();
    MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
    for (;;) {
        if (observed == nullptr) {
            hazard.store(nullptr, std::memory_order_release);
            return nullptr;
        }

        // Publish before dereferencing observed->Next(). The full fence and
        // acquire reload are the ZGC zMarkStack.cpp:98-121 handshake: either a
        // reclaimer sees this hazard or we see that the head changed.
        hazard.store(observed, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        MarkStripeStackListNode* const afterPublish = head.load(std::memory_order_acquire);
        if (afterPublish != observed) {
            observed = afterPublish;
            continue;
        }

        MarkStripeStackListNode* const next = observed->Next();
        if (head.compare_exchange_strong(observed, next, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            hazard.store(nullptr, std::memory_order_release);
            length.fetch_sub(1, std::memory_order_relaxed);
            MarkStripeStack* const stack = observed->Stack();
            smr.free_node(observed);
            return stack;
        }
    }
}

MarkStripeStack* MarkStripe::StealStack(MarkingSMR& smr, size_t workerId)
{
    MarkStripeStack* const overflow = overflowed.Pop(smr, workerId);
    return overflow != nullptr ? overflow : published.Pop(smr, workerId);
}

MarkStripeSet::MarkStripeSet(size_t stripeCount)
    : capacityMask(stripeCount - 1), nstripesMask(stripeCount - 1)
{
    CHECK_DETAIL(IsPowerOfTwo(stripeCount), "mark stripe count must be a power of two: %zu", stripeCount);
    stripes.reserve(stripeCount);
    for (size_t i = 0; i < stripeCount; ++i) {
        stripes.emplace_back(new (std::nothrow) MarkStripe());
        CHECK_DETAIL(stripes.back() != nullptr, "failed to allocate mark stripe index=%zu", i);
    }
}

void MarkStripeSet::SetNStripes(size_t value)
{
    CHECK_DETAIL(IsPowerOfTwo(value) && value <= stripes.size(),
                 "nstripes=%zu must be power of two within capacity=%zu", value, stripes.size());
    nstripesMask.store(value - 1, std::memory_order_relaxed);
}

bool MarkStripeSet::TrySetNStripes(size_t oldNStripes, size_t newNStripes)
{
    CHECK_DETAIL(IsPowerOfTwo(newNStripes) && newNStripes >= 1 && newNStripes <= stripes.size(),
                 "nstripes=%zu must be power of two within capacity=%zu", newNStripes, stripes.size());
    size_t expected = oldNStripes - 1;
    return nstripesMask.compare_exchange_strong(expected, newNStripes - 1, std::memory_order_relaxed);
}

size_t MarkStripeSet::CalculateNStripes(size_t nworkers) const
{
    constexpr size_t multiplier = 4;
    size_t target = std::max(nworkers * multiplier, multiplier);
    size_t count = 1;
    while (count < target && count < stripes.size()) {
        count <<= 1;
    }
    return count;
}

bool MarkStripeSet::IsCrowded() const
{
    size_t population = 0;
    const size_t crowdedThreshold = NStripes() << 4;
    for (const auto& stripe : stripes) {
        population += stripe->Population();
        if (population > crowdedThreshold) {
            return true;
        }
    }
    return false;
}

bool MarkStripeSet::IsEmpty() const
{
    for (const auto& stripe : stripes) {
        if (!stripe->IsEmpty()) {
            return false;
        }
    }
    return true;
}

size_t MarkStripeSet::Population() const
{
    size_t population = 0;
    for (const auto& stripe : stripes) {
        population += stripe->Population();
    }
    return population;
}

size_t MarkStripeSet::FirstNonEmptyStripe() const
{
    for (size_t i = 0; i < stripes.size(); ++i) {
        if (!stripes[i]->IsEmpty()) {
            return i;
        }
    }
    return std::numeric_limits<size_t>::max();
}

size_t MarkStripeSet::StripeForWorker(size_t nworkers, size_t workerId) const
{
    CHECK_DETAIL(nworkers != 0 && workerId < nworkers, "invalid mark worker id=%zu count=%zu", workerId,
                 nworkers);
    const size_t mask = NStripesMask();
    const size_t active = mask + 1;
    const size_t spilloverLimit = (nworkers / active) * active;
    if (workerId < spilloverLimit) {
        return workerId & mask;
    }
    const size_t spilloverWorkers = nworkers - spilloverLimit;
    const size_t spilloverId = workerId - spilloverLimit;
    return static_cast<size_t>(static_cast<double>(spilloverId) *
                               (static_cast<double>(active) / static_cast<double>(spilloverWorkers)));
}

MarkThreadLocalStacks::MarkThreadLocalStacks(size_t stripeCount) : stacks(stripeCount, nullptr) {}

MarkThreadLocalStacks::~MarkThreadLocalStacks()
{
    for (MarkStripeStack* stack : stacks) {
        MarkStripeStack::Destroy(stack);
    }
}

bool MarkThreadLocalStacks::IsEmpty() const
{
    for (MarkStripeStack* stack : stacks) {
        if (stack != nullptr) {
            return false;
        }
    }
    return true;
}

size_t MarkThreadLocalStacks::Population() const
{
    size_t population = 0;
    for (MarkStripeStack* stack : stacks) {
        if (stack != nullptr) {
            // ZGC treats an installed (even empty) local chunk as non-empty;
            // normally Pop destroys it immediately, so one here is a leaked
            // ownership receipt rather than an object count.
            population += stack->IsEmpty() ? 1 : stack->Size();
        }
    }
    return population;
}

bool MarkThreadLocalStacks::Flush(MarkStripeSet& stripes)
{
    bool flushed = false;
    for (size_t i = 0; i < stacks.size(); ++i) {
        MarkStripeStack*& stack = stacks[i];
        if (stack == nullptr) {
            continue;
        }
        stripes.At(i).PublishStack(stack, true, stripes.Terminate());
        stack = nullptr;
        flushed = true;
    }
    return flushed;
}

MarkStripeStackListNode::MarkStripeStackListNode(MarkStripeStack* stack) : stack(stack) {}

MarkStripeStack* MarkStripeStackListNode::Stack() const { return stack; }

MarkStripeStackListNode* MarkStripeStackListNode::Next() const { return next; }

void MarkStripeStackListNode::SetNext(MarkStripeStackListNode* value) { next = value; }

bool MarkStripeStackList::IsEmpty() const { return head.load(std::memory_order_acquire) == nullptr; }

size_t MarkStripe::Population() const { return published.Length() + overflowed.Length(); }

size_t MarkStripeSet::NStripes() const { return nstripesMask.load(std::memory_order_relaxed) + 1; }


} // namespace MapleRuntime

#include "Heap/z/zMarkContext.inline.hpp"

#include "Heap/z/zMarkStack.inline.hpp"
