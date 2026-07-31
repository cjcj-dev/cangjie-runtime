// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "IdleLogBarrier.h"

#include "Heap/Heap.h"
#include "Heap/RemsetCheck.h"
#include "Heap/StickyLog.h"
#include "ObjectModel/MArray.h"

namespace MapleRuntime {
ALWAYS_INLINE void IdleLogBarrier::LogObject(BaseObject* obj) const { CJ_MCC_StickyLogLine(obj); }

void IdleLogBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::WRITE_REFERENCE);
    LogObject(obj);
    IdleBarrier::WriteReference(obj, field, ref);
    check.RecordBarrierEdge(obj, reinterpret_cast<MAddress>(&field), ref,
                            RemsetCheck::HookSite::WRITE_REFERENCE);
}

void IdleLogBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    IdleBarrier::WriteStaticRef(field, ref);
}

void IdleLogBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::WRITE_STRUCT);
    LogObject(obj);
    IdleBarrier::WriteStruct(obj, dst, dstLen, src, srcLen);
    if (check.IsEnabled() && obj != nullptr && Heap::IsHeapAddress(obj)) {
        obj->ForEachRefInStruct(
            [&check, obj](RefField<false>& field) {
                check.RecordBarrierEdge(obj, reinterpret_cast<MAddress>(&field), field.GetTargetObject(),
                                        RemsetCheck::HookSite::WRITE_STRUCT);
            },
            dst, dst + dstLen);
    }
}

void IdleLogBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                          MemoryOrder order) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::ATOMIC_WRITE_REFERENCE);
    LogObject(obj);
    IdleBarrier::AtomicWriteReference(obj, field, ref, order);
    check.RecordBarrierEdge(obj, reinterpret_cast<MAddress>(&field), ref,
                            RemsetCheck::HookSite::ATOMIC_WRITE_REFERENCE);
}

BaseObject* IdleLogBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                                MemoryOrder order) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::ATOMIC_SWAP_REFERENCE);
    LogObject(obj);
    BaseObject* oldRef = IdleBarrier::AtomicSwapReference(obj, field, ref, order);
    check.RecordBarrierEdge(obj, reinterpret_cast<MAddress>(&field), ref,
                            RemsetCheck::HookSite::ATOMIC_SWAP_REFERENCE);
    return oldRef;
}

bool IdleLogBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::COMPARE_AND_SWAP_REFERENCE);
    LogObject(obj);
    bool exchanged = IdleBarrier::CompareAndSwapReference(obj, field, oldRef, newRef, succOrder, failOrder);
    if (exchanged) {
        check.RecordBarrierEdge(obj, reinterpret_cast<MAddress>(&field), newRef,
                                RemsetCheck::HookSite::COMPARE_AND_SWAP_REFERENCE);
    }
    return exchanged;
}

void IdleLogBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                     MAddress srcField, MIndex srcSize) const
{
    RemsetCheck& check = RemsetCheck::Instance();
    check.RecordHookHit(RemsetCheck::HookSite::COPY_STRUCT_ARRAY);
    LogObject(dstObj);
    IdleBarrier::CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    if (check.IsEnabled() && dstObj != nullptr && Heap::IsHeapAddress(dstObj)) {
        MArray* dstArray = static_cast<MArray*>(dstObj);
        dstArray->ForEachRefFieldInRange(
            [&check, dstObj](RefField<false>& field) {
                check.RecordBarrierEdge(dstObj, reinterpret_cast<MAddress>(&field), field.GetTargetObject(),
                                        RemsetCheck::HookSite::COPY_STRUCT_ARRAY);
            },
            dstField, dstField + dstSize);
    }
}

void IdleLogBarrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    RemsetCheck::Instance().RecordHookHit(RemsetCheck::HookSite::WRITE_GENERIC);
    IdleBarrier::WriteGeneric(obj, fieldPtr, src, size);
}
} // namespace MapleRuntime
