// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_TRACE_FIX_BARRIER_H
#define MRT_TRACE_FIX_BARRIER_H

#include "Allocator/RegionSpace.h"
#include "IdleBarrier.h"

namespace MapleRuntime {
// PreforwardBarrier is the barrier for concurrent copying gc in fixup stage
class PreforwardBarrier : public IdleBarrier {
    friend class Barrier;
public:
    PreforwardBarrier(Collector& collector, RememberedSet& rememberedSet)
        : IdleBarrier(collector, rememberedSet, BarrierPhase::PREFORWARD) {}

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    BaseObject* ReadStaticRef(ReadOnlyRootSlot& field) const;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;

    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const;
protected:
    void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                  MemoryOrder order) const;
    BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                        MemoryOrder order) const;
    bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                     MemoryOrder succOrder, MemoryOrder failOrder) const;
    void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                             MAddress srcField, MIndex srcSize) const;
};
} // namespace MapleRuntime
#endif // MRT_TRACE_FIX_BARRIER_H
