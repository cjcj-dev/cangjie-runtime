// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Common/TypeDef.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
namespace MapleRuntime {
// gc/shared/memAllocator.cpp:327-350: TLAB selection precedes heap allocation.
class ThreadLocalMemAllocator {
public:
    ThreadLocalMemAllocator(size_t size, AllocType type) : _size(size), _type(type) {}
    MAddress allocate() const;
private:
    MAddress mem_allocate() const;
    MAddress mem_allocate_inside_tlab_fast() const;
    MAddress mem_allocate_inside_tlab_slow() const;
    MAddress mem_allocate_outside_tlab() const;
    const size_t _size;
    const AllocType _type;
};
}
