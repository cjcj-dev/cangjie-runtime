// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "IdleLogBarrier.h"

#include "Heap/StickyLog.h"
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
#include "Heap/EmitSiteCounters.h"
#endif

namespace MapleRuntime {
ALWAYS_INLINE void IdleLogBarrier::LogObject(BaseObject* obj) const { CJ_MCC_StickyLogLine(obj); }

void IdleLogBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    LogObject(obj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, obj, ref, true);
#endif
    IdleBarrier::WriteReference(obj, field, ref);
}

void IdleLogBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    IdleBarrier::WriteStaticRef(field, ref);
}

void IdleLogBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    LogObject(obj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    // struct write: count each dst ref that points young from old holder after copy
    // (post-copy scan would need ForEach; approximate via holder+null ref for exercised)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, obj, nullptr, true);
#endif
    IdleBarrier::WriteStruct(obj, dst, dstLen, src, srcLen);
}

void IdleLogBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                          MemoryOrder order) const
{
    LogObject(obj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, obj, ref, true);
#endif
    IdleBarrier::AtomicWriteReference(obj, field, ref, order);
}

BaseObject* IdleLogBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                                MemoryOrder order) const
{
    LogObject(obj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, obj, ref, true);
#endif
    return IdleBarrier::AtomicSwapReference(obj, field, ref, order);
}

bool IdleLogBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    LogObject(obj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, obj, newRef, true);
#endif
    return IdleBarrier::CompareAndSwapReference(obj, field, oldRef, newRef, succOrder, failOrder);
}

void IdleLogBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                     MAddress srcField, MIndex srcSize) const
{
    LogObject(dstObj);
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
    EmitSiteNoteWrite(EmitBarrierKind::IdleLog, dstObj, nullptr, true);
#endif
    IdleBarrier::CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void IdleLogBarrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    IdleBarrier::WriteGeneric(obj, fieldPtr, src, size);
}
} // namespace MapleRuntime
