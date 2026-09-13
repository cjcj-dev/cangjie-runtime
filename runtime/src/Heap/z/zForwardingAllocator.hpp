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
// The generation relocation set owns the arena through its reset boundary.
class ForwardingAllocator {
public:
    explicit ForwardingAllocator(size_t capacity);
    ~ForwardingAllocator();
    ForwardingAllocator(const ForwardingAllocator&) = delete;
    ForwardingAllocator& operator=(const ForwardingAllocator&) = delete;

    static bool aligned_size(size_t size, size_t* aligned);

    static bool add_to_budget(size_t size, size_t* budget);

    bool valid() const;
    size_t capacity() const;
#if defined(MRT_TESTABLE_INTERNALS)
    bool contains_for_test(const void* address, size_t size) const
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(start_);
        const uintptr_t at = reinterpret_cast<uintptr_t>(address);
        return at >= start && at - start <= capacity_ && size <= capacity_ - (at - start);
    }
#endif
    size_t used() const;

    void* allocate(size_t size);

private:
    void* const start_;
    const size_t capacity_;
    std::atomic<size_t> top_;
};

} // namespace MapleRuntime
#include "Heap/z/zForwardingAllocator.inline.hpp"

#endif // MRT_FORWARDING_ALLOCATOR_H
