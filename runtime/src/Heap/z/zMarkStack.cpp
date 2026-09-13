#include "Heap/z/zUtils.inline.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <limits>

#include <algorithm>
#include <new>
#include <type_traits>

#include "Base/Log.h"
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
namespace {
constexpr size_t FIRST_STACK_CAPACITY = 128;
constexpr size_t REGULAR_STACK_CAPACITY = 512;
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<MarkStripeStack::StorageObserver> storageObserver{nullptr};
#endif

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
#if defined(MRT_TESTABLE_INTERNALS)
    if (const auto observer = storageObserver.load(std::memory_order_acquire)) {
        observer(stack, stack->entries(stack), capacity, true);
    }
#endif
    return stack;
}

void MarkStripeStack::Destroy(MarkStripeStack* stack)
{
    // Local slots can be null, unlike the non-null-only ZGC destroy caller.
    if (stack == nullptr) {
        return;
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (const auto observer = storageObserver.load(std::memory_order_acquire)) {
        observer(stack, stack->entries(stack), stack->Capacity(), false);
    }
#endif
    stack->~MarkStripeStack();
    AttachedArray::free(stack);
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<MarkClosureObserver> g_markClosureObserver{nullptr};
}
void SetMarkClosureObserverForTest(MarkClosureObserver observer)
{
    g_markClosureObserver.store(observer, std::memory_order_release);
}
void ObserveMarkClosureForTest(const std::vector<BaseObject*>* objects)
{
    auto observer = g_markClosureObserver.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(objects);
    }
}
#endif

} // namespace MapleRuntime

#include "Heap/z/zMarkContext.inline.hpp"

#include "Heap/z/zMarkStack.inline.hpp"
