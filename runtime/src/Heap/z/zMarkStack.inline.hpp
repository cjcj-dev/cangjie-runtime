// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"
namespace MapleRuntime {
bool MarkStripeStack::IsEmpty() const { return top == 0; }
bool MarkStripeStack::IsFull() const { return top == entries.length(); }
size_t MarkStripeStack::Size() const { return top; }
size_t MarkStripeStack::Capacity() const { return entries.length(); }
bool MarkStripe::IsEmpty() const { return published.IsEmpty() && overflowed.IsEmpty(); }
size_t MarkStripeSet::StripeId(const MarkStripe* stripe) const
{
    const size_t index = (reinterpret_cast<uintptr_t>(stripe) -
                          reinterpret_cast<uintptr_t>(stripes.data())) / sizeof(MarkStripe);
    CHECK_DETAIL(index < stripes.size(), "invalid mark stripe index=%zu", index);
    return index;
}
MarkStripe* MarkStripeSet::Next(MarkStripe* stripe) { return At((StripeId(stripe) + 1) & capacityMask); }
MarkStripe* MarkStripeSet::At(size_t stripeId)
{
    CHECK_DETAIL(stripeId < stripes.size(), "invalid mark stripe index=%zu", stripeId);
    return &stripes[stripeId];
}
const MarkStripe* MarkStripeSet::At(size_t stripeId) const
{
    CHECK_DETAIL(stripeId < stripes.size(), "invalid mark stripe index=%zu", stripeId);
    return &stripes[stripeId];
}





void MarkStripeStack::Push(const MarkStackEntry& entry)
{
    CHECK_DETAIL(!IsFull(), "cannot push to a full mark stripe stack");
    entries(this)[top++] = entry;
}

MarkStackEntry MarkStripeStack::Pop()
{
    CHECK_DETAIL(!IsEmpty(), "cannot pop from an empty mark stripe stack");
    return entries(this)[--top];
}







void MarkStripe::PublishStack(MarkStripeStack* stack, bool publish, MarkTerminate* terminate)
{
    CHECK_DETAIL(!stack->IsEmpty(), "never publish an empty mark stripe stack");
    if (publish) {
        published.Push(stack);
    } else {
        overflowed.Push(stack);
    }
    terminate->Wake();
}



















MarkStripe* MarkStripeSet::StripeForAddress(uintptr_t address)
{
    return At((address >> MARK_STRIPE_SHIFT) & NStripesMask());
}











void MarkThreadLocalStacks::Push(MarkStripeSet& stripes, MarkStripe* stripe, const MarkStackEntry& entry,
                                 bool publish)
{
    const size_t stripeId = stripes.StripeId(stripe);
    CHECK_DETAIL(stripeId < stacks.size(), "invalid local mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    MarkStripeStack* const previous = slot;
    if (previous != nullptr) {
        if (!previous->IsFull()) {
            previous->Push(entry);
            return;
        }
        stripe->PublishStack(previous, publish, stripes.Terminate());
        slot = nullptr;
    }

    slot = MarkStripeStack::Create(previous == nullptr);
    CHECK_DETAIL(slot != nullptr, "failed to allocate local mark stripe stack");
    slot->Push(entry);
}

bool MarkThreadLocalStacks::Pop(MarkingSMR& smr, size_t workerId, MarkStripeSet& stripes, MarkStripe* stripe,
                                MarkStackEntry& entry)
{
    const size_t stripeId = stripes.StripeId(stripe);
    CHECK_DETAIL(stripeId < stacks.size(), "invalid pop mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    if (slot == nullptr) {
        slot = stripe->StealStack(smr, workerId);
        if (slot == nullptr) {
            return false;
        }
    }
    entry = slot->Pop();
    if (slot->IsEmpty()) {
        MarkStripeStack::Destroy(slot);
        slot = nullptr;
    }
    return true;
}

MarkStripeStack* MarkThreadLocalStacks::StealLocal(MarkStripeSet& stripes, MarkStripe* stripe)
{
    const size_t stripeId = stripes.StripeId(stripe);
    CHECK_DETAIL(stripeId < stacks.size(), "invalid steal-local stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack* const result = stacks[stripeId];
    stacks[stripeId] = nullptr;
    return result;
}

void MarkThreadLocalStacks::Install(MarkStripeSet& stripes, MarkStripe* stripe, MarkStripeStack* stack)
{
    const size_t stripeId = stripes.StripeId(stripe);
    CHECK_DETAIL(stripeId < stacks.size(), "invalid install stripe=%zu count=%zu", stripeId, stacks.size());
    CHECK_DETAIL(stacks[stripeId] == nullptr, "install target stripe=%zu is not empty", stripeId);
    stacks[stripeId] = stack;
}



}
