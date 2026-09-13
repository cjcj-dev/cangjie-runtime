// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zForwardingAllocator.hpp"

namespace MapleRuntime {
inline bool ForwardingAllocator::aligned_size(size_t size, size_t* aligned)
{
        constexpr size_t alignment = alignof(std::max_align_t);
        if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
            return false;
        }
        *aligned = (size + alignment - 1) & ~(alignment - 1);
        return true;
    }
}

namespace MapleRuntime {
inline bool ForwardingAllocator::add_to_budget(size_t size, size_t* budget)
{
        size_t aligned;
        if (!aligned_size(size, &aligned) || aligned > std::numeric_limits<size_t>::max() - *budget) {
            return false;
        }
        *budget += aligned;
        return true;
    }
}

namespace MapleRuntime {
inline bool ForwardingAllocator::valid() const
{ return start_ != nullptr || capacity_ == 0; }
}

namespace MapleRuntime {
inline size_t ForwardingAllocator::capacity() const
{ return capacity_; }
}

namespace MapleRuntime {
inline size_t ForwardingAllocator::used() const
{ return top_.load(std::memory_order_relaxed); }
}

namespace MapleRuntime {
inline void* ForwardingAllocator::allocate(size_t size)
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
}
