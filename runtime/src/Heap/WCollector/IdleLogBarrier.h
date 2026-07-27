// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_IDLE_LOG_BARRIER_H
#define MRT_IDLE_LOG_BARRIER_H

#include "IdleBarrier.h"

namespace MapleRuntime {
class IdleLogBarrier : public IdleBarrier {
public:
    explicit IdleLogBarrier(Collector& collector) : IdleBarrier(collector) {}

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override;
    void WriteStaticRef(RefField<false>& field, BaseObject* ref) const override;
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const override;
    void AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                              MemoryOrder order) const override;
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                    MemoryOrder order) const override;
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const override;
    void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                         MIndex srcSize) const override;
    void WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const override;

private:
    void LogObject(BaseObject* obj) const;
};
} // namespace MapleRuntime
#endif // MRT_IDLE_LOG_BARRIER_H
