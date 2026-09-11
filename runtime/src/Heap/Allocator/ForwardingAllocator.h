// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_ALLOCATOR_H
#define MRT_FORWARDING_ALLOCATOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace MapleRuntime {

// zForwardingAllocator.cpp:36-46 / zForwardingAllocator.inline.hpp:32-42.
// One budgeted allocation, stable addresses, monotonic parallel allocation.
// Unlike ZGC's set-wide reset, old carriers can retire independently in the
// transitional collector. Their owners keep this arena alive until the last
// carrier is destroyed; the arena is never resized under a reader.
class ForwardingAllocator {
public:
    explicit ForwardingAllocator(size_t capacity)
        : start_(capacity == 0 ? nullptr : std::malloc(capacity)), capacity_(capacity), top_(0) {}
    ~ForwardingAllocator() { std::free(start_); }
    ForwardingAllocator(const ForwardingAllocator&) = delete;
    ForwardingAllocator& operator=(const ForwardingAllocator&) = delete;

    static bool aligned_size(size_t size, size_t* aligned)
    {
        constexpr size_t alignment = alignof(std::max_align_t);
        if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
            return false;
        }
        *aligned = (size + alignment - 1) & ~(alignment - 1);
        return true;
    }

    static bool add_to_budget(size_t size, size_t* budget)
    {
        size_t aligned;
        if (!aligned_size(size, &aligned) || aligned > std::numeric_limits<size_t>::max() - *budget) {
            return false;
        }
        *budget += aligned;
        return true;
    }

    bool valid() const { return start_ != nullptr || capacity_ == 0; }
    size_t capacity() const { return capacity_; }
#if defined(MRT_TESTABLE_INTERNALS)
    bool contains_for_test(const void* address, size_t size) const
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(start_);
        const uintptr_t at = reinterpret_cast<uintptr_t>(address);
        return at >= start && at - start <= capacity_ && size <= capacity_ - (at - start);
    }
#endif
    size_t used() const { return top_.load(std::memory_order_relaxed); }

    void* allocate(size_t size)
    {
        size_t aligned;
        if (start_ == nullptr || size == 0 || !aligned_size(size, &aligned)) {
            return nullptr;
        }
        size_t top = top_.load(std::memory_order_relaxed);
        for (;;) {
            if (aligned > capacity_ - top) {
                return nullptr;
            }
            if (top_.compare_exchange_weak(top, top + aligned, std::memory_order_relaxed)) {
                return static_cast<char*>(start_) + top;
            }
        }
    }

private:
    void* const start_;
    const size_t capacity_;
    std::atomic<size_t> top_;
};

} // namespace MapleRuntime
#endif // MRT_FORWARDING_ALLOCATOR_H
