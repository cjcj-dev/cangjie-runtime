// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.

#ifndef MRT_FORWARDING_ALLOCATOR_H
#define MRT_FORWARDING_ALLOCATOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace MapleRuntime {

class ZForwardingAllocator {
public:
    ZForwardingAllocator();
    explicit ZForwardingAllocator(size_t size) : ZForwardingAllocator() { reset(size); }
    ~ZForwardingAllocator();
    ZForwardingAllocator(const ZForwardingAllocator&) = delete;
    ZForwardingAllocator& operator=(const ZForwardingAllocator&) = delete;

    void reset(size_t size);
    size_t size() const;
    bool is_full() const;
    void* alloc(size_t size);

    static bool aligned_size(size_t size, size_t* aligned);
    static bool add_to_budget(size_t size, size_t* budget);
    bool valid() const;
    size_t capacity() const { return size(); }
    size_t used() const;
    void* allocate(size_t size) { return alloc(size); }

private:
    char* _start;
    char* _end;
    std::atomic<char*> _top;
};

using ForwardingAllocator = ZForwardingAllocator;

} // namespace MapleRuntime
#include "Heap/z/zForwardingAllocator.inline.hpp"
#endif
