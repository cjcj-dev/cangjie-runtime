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
size_t MarkStripeSet::Next(size_t stripeId) const { return (stripeId + 1) & capacityMask; }
size_t MarkStripeSet::Next(size_t stripeId, size_t offset) const { return (stripeId + offset) & capacityMask; }
MarkStripe& MarkStripeSet::At(size_t stripeId) { return *stripes[stripeId]; }
const MarkStripe& MarkStripeSet::At(size_t stripeId) const { return *stripes[stripeId]; }





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
    if (publish) {
        published.Push(stack);
    } else {
        overflowed.Push(stack);
    }
    if (terminate != nullptr) {
        terminate->Wake();
    }
}



















size_t MarkStripeSet::StripeForAddress(uintptr_t address) const
{
    return (address >> MARK_STRIPE_SHIFT) & NStripesMask();
}











void MarkThreadLocalStacks::Push(MarkStripeSet& stripes, size_t stripeId, const MarkStackEntry& entry,
                                 bool publish)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid local mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    MarkStripeStack* const previous = slot;
    if (previous != nullptr) {
        if (!previous->IsFull()) {
            previous->Push(entry);
            return;
        }
        stripes.At(stripeId).PublishStack(previous, publish, stripes.Terminate());
        slot = nullptr;
    }

    slot = MarkStripeStack::Create(previous == nullptr);
    CHECK_DETAIL(slot != nullptr, "failed to allocate local mark stripe stack");
    slot->Push(entry);
}

bool MarkThreadLocalStacks::Pop(MarkingSMR& smr, size_t workerId, MarkStripeSet& stripes, size_t stripeId,
                                MarkStackEntry& entry)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid pop mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    if (slot == nullptr) {
        slot = stripes.At(stripeId).StealStack(smr, workerId);
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

MarkStripeStack* MarkThreadLocalStacks::StealLocal(size_t stripeId)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid steal-local stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack* const result = stacks[stripeId];
    stacks[stripeId] = nullptr;
    return result;
}

void MarkThreadLocalStacks::Install(size_t stripeId, MarkStripeStack* stack)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid install stripe=%zu count=%zu", stripeId, stacks.size());
    CHECK_DETAIL(stacks[stripeId] == nullptr, "install target stripe=%zu is not empty", stripeId);
    stacks[stripeId] = stack;
}



}
