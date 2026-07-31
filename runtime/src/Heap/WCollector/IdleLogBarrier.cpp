// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "IdleLogBarrier.h"

#include "Heap/StickyLog.h"

namespace MapleRuntime {
ALWAYS_INLINE void IdleLogBarrier::LogObject(BaseObject* obj) const { CJ_MCC_StickyLogLine(obj); }

void IdleLogBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    StickyLog::Instance().RecordEdgeCompleteStoreCandidate(obj, reinterpret_cast<MAddress>(&field), ref);
    LogObject(obj);
    IdleBarrier::WriteReference(obj, field, ref);
}

void IdleLogBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    IdleBarrier::WriteStaticRef(field, ref);
}

void IdleLogBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    LogObject(obj);
    IdleBarrier::WriteStruct(obj, dst, dstLen, src, srcLen);
}

void IdleLogBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                          MemoryOrder order) const
{
    StickyLog::Instance().RecordEdgeCompleteStoreCandidate(obj, reinterpret_cast<MAddress>(&field), ref);
    LogObject(obj);
    IdleBarrier::AtomicWriteReference(obj, field, ref, order);
}

BaseObject* IdleLogBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                                MemoryOrder order) const
{
    StickyLog::Instance().RecordEdgeCompleteStoreCandidate(obj, reinterpret_cast<MAddress>(&field), ref);
    LogObject(obj);
    return IdleBarrier::AtomicSwapReference(obj, field, ref, order);
}

bool IdleLogBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    LogObject(obj);
    return IdleBarrier::CompareAndSwapReference(obj, field, oldRef, newRef, succOrder, failOrder);
}

void IdleLogBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                     MAddress srcField, MIndex srcSize) const
{
    LogObject(dstObj);
    IdleBarrier::CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void IdleLogBarrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    IdleBarrier::WriteGeneric(obj, fieldPtr, src, size);
}
} // namespace MapleRuntime
