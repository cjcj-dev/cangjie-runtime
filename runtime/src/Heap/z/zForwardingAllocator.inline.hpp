#pragma once
#include "Heap/z/zForwardingAllocator.hpp"
#include "Base/Log.h"

namespace MapleRuntime {

inline bool ZForwardingAllocator::aligned_size(size_t size, size_t* aligned)
{
    constexpr size_t alignment = alignof(std::max_align_t);
    if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return false;
    }
    *aligned = (size + alignment - 1) & ~(alignment - 1);
    return true;
}

inline bool ZForwardingAllocator::add_to_budget(size_t size, size_t* budget)
{
    size_t aligned;
    if (!aligned_size(size, &aligned) || aligned > std::numeric_limits<size_t>::max() - *budget) {
        return false;
    }
    *budget += aligned;
    return true;
}

inline size_t ZForwardingAllocator::size() const { return static_cast<size_t>(_end - _start); }

inline bool ZForwardingAllocator::is_full() const { return _top.load(std::memory_order_relaxed) == _end; }

inline bool ZForwardingAllocator::valid() const { return _start != nullptr || size() == 0; }

inline size_t ZForwardingAllocator::used() const
{
    return static_cast<size_t>(_top.load(std::memory_order_relaxed) - _start);
}

inline void* ZForwardingAllocator::alloc(size_t size)
{
    char* const addr = _top.fetch_add(size, std::memory_order_relaxed);
    CHECK(addr + size <= _end);
    return addr;
}

#if defined(MRT_TESTABLE_INTERNALS)
inline bool ZForwardingAllocator::contains_for_test(const void* address, size_t nbytes) const
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(_start);
    const uintptr_t at = reinterpret_cast<uintptr_t>(address);
    const size_t cap = size();
    return at >= start && at - start <= cap && nbytes <= cap - (at - start);
}
#endif

} // namespace MapleRuntime
