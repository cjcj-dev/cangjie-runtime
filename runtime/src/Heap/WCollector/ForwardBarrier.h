// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COPY_BARRIER_H
#define MRT_COPY_BARRIER_H

#include "IdleBarrier.h"

namespace MapleRuntime {
// ForwardBarrier is the barrier for concurrent forwarding.
class ForwardBarrier : public IdleBarrier {
public:
    ForwardBarrier(Collector& collector, RememberedSet& rememberedSet) : IdleBarrier(collector, rememberedSet) {}

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override;
    BaseObject* ReadStaticRef(RefField<false>& field) const override;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override;

    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const override;
protected:
    void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                  MemoryOrder order) const override;
    BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                        MemoryOrder order) const override;
    bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                     MemoryOrder succOrder, MemoryOrder failOrder) const override;
    void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                             MAddress srcField, MIndex srcSize) const override;
};
} // namespace MapleRuntime
#endif // MRT_COPY_BARRIER_H
