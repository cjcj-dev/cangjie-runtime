// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/shared/memAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
namespace MapleRuntime {
MAddress MemAllocator::mem_allocate_inside_tlab_fast() const
{
    return ThreadLocal::GetMutator()->tlab()->Allocate(_size, _type);
}
MAddress MemAllocator::mem_allocate_inside_tlab_slow() const
{
    return ThreadLocal::GetMutator()->tlab()->AllocateImpl(_size, _type);
}
MAddress MemAllocator::mem_allocate_outside_tlab() const
{
    return Heap::GetHeap().Allocate(_size, _type);
}
MAddress MemAllocator::mem_allocate() const
{
    // Native callers without a mutator have no TLAB.
    const bool useTLAB = _size <= ZObjectSizeLimitSmall && ThreadLocal::GetMutator() != nullptr;
    if (useTLAB) {
        const MAddress addr = mem_allocate_inside_tlab_fast();
        if (addr != 0) { return addr; }
    }
    if (useTLAB) {
        const MAddress addr = mem_allocate_inside_tlab_slow();
        if (addr != 0) { return addr; }
    }
    return mem_allocate_outside_tlab();
}
MAddress MemAllocator::allocate() const
{
    const MAddress addr = mem_allocate();
#if defined(CANGJIE_TSAN_SUPPORT)
    if (addr != 0) { Sanitizer::TsanAllocObject(reinterpret_cast<void*>(addr), _size); }
#endif
    return addr;
}
}
