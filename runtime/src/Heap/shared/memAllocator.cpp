// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/shared/memAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Mutator/Mutator.inline.h"
#include "Base/MemUtils.h"
#include "schedule_rename.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
namespace MapleRuntime {
MAddress MemAllocator::mem_allocate_inside_tlab_fast() const
{
    return ThreadLocal::GetMutator()->tlab()->Allocate(_size, _type);
}
// gc/shared/memAllocator.cpp:255-325: the refill schedule (retain, retire,
// compute_size, allocate_new_tlab, clear, fill) belongs to this function.
// gc/shared/threadLocalAllocBuffer.hpp:142,152,167,170 only provides the
// basic operations it calls; the TLAB class has no allocation dispatch.
MAddress MemAllocator::mem_allocate_inside_tlab_slow() const
{
    AllocBuffer* tlab = ThreadLocal::GetMutator()->tlab();
    // Retain the tlab and allocate the object outside it when the free amount
    // is too large to discard.
    if (tlab->TLABFree() > tlab->RefillWasteLimit()) {
        tlab->RecordSlowAllocation(_size);
        return 0;
    }
    // Discard the tlab and allocate a new one.
    tlab->RetireTLAB(false);
    const size_t tlabSize = tlab->ComputeTLABSize(_size, Heap::GetHeap().unsafe_max_tlab_alloc());
    if (tlabSize == 0) { return 0; }
    // Cangjie tasks can migrate while page allocation enters a saferegion.
    CJThreadPreemptOffCntAdd();
    size_t actualSize = 0;
    const uintptr_t start = ZCollectedHeap::heap()->allocate_new_tlab(_size, tlabSize, &actualSize);
    CJThreadPreemptOffCntSub();
    if (start == 0) { return 0; }
    // Clear the new TLAB before its bounds are published.
    MemorySet(start, actualSize, 0, actualSize);
    tlab->FillTLAB(start, actualSize);
    return tlab->Allocate(_size, _type);
}
MAddress MemAllocator::mem_allocate_outside_tlab() const
{
    return Heap::GetHeap().Allocate(_size, _type);
}
MAddress MemAllocator::mem_allocate() const
{
    // Native callers without a mutator have no TLAB; that is the only
    // adaptation, gc/shared/memAllocator.cpp:327-350 only splits on UseTLAB.
    const bool useTLAB = ThreadLocal::GetMutator() != nullptr;
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
