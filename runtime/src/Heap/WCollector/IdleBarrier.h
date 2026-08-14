// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_IDLE_BARRIER_H
#define MRT_IDLE_BARRIER_H

#include "Barrier/Barrier.h"

namespace MapleRuntime {
// IdleBarrier is the barrier for concurrent enum phase
class IdleBarrier : public Barrier {
    friend class Barrier;
public:
    IdleBarrier(Collector& collector, RememberedSet& rememberedSet)
        : IdleBarrier(collector, rememberedSet, BarrierPhase::IDLE) {}

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    BaseObject* ReadStaticRef(ReadOnlyRootSlot& field) const;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;
    void WriteStaticRef(RootSlot& field, BaseObject* ref) const;
    void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const;

    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const;
protected:
    IdleBarrier(Collector& collector, RememberedSet& rememberedSet, BarrierPhase phase)
        : Barrier(collector, rememberedSet, phase) {}

    void WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    void WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                  MemoryOrder order) const;
    BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                        MemoryOrder order) const;
    bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                     MemoryOrder succOrder, MemoryOrder failOrder) const;
    void CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                          MIndex srcSize) const;
    void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                             MAddress srcField, MIndex srcSize) const;
};
} // namespace MapleRuntime
#endif // MRT_IDLE_BARRIER_H
