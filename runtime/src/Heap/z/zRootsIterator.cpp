
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void ResetSkippedStackMapCounts();
void RecordRootMapMiss(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP, uintptr_t frameIP,
                       const Mutator& mutator);
ATTR_NO_INLINE void RecordSkippedStackMap(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP,
                                          uintptr_t frameIP);
void ReportSkippedStackMapCounts();

// Fill gc roots entry to buckets
void StaticRootTable::RegisterRoots(StaticRootArray* addr, U32 size)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    // L741: map::insert keeps first value; must not inflate totalRootsCount on dup key.
    auto result = gcRootsBuckets.insert(std::pair<StaticRootArray*, U32>(addr, size));
    if (!result.second) {
        LOG(RTLOG_ERROR,
            "StaticRootTable::RegisterRoots duplicate key %p size %u (kept size %u); totalRootsCount not increased",
            addr, size, result.first->second);
        return;
    }
    totalRootsCount += size;
}

void StaticRootTable::UnregisterRoots(StaticRootArray* addr, U32 size)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    auto iter = gcRootsBuckets.find(addr);
    if (iter == gcRootsBuckets.end()) {
        LOG(RTLOG_ERROR, "StaticRootTable::UnregisterRoots missing key %p size %u", addr, size);
        return;
    }
    if (iter->second != size) {
        LOG(RTLOG_ERROR,
            "StaticRootTable::UnregisterRoots size mismatch key %p caller %u registered %u; using registered",
            addr, size, iter->second);
        totalRootsCount -= iter->second;
    } else {
        totalRootsCount -= size;
    }
    gcRootsBuckets.erase(iter);
}

void StaticRootTable::VisitRoots(const NativeSlotVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(gcRootsLock);
    U32 gcRootsSize = 0;
    std::unordered_set<NativeSlot*> visitedSet;
    for (auto iter = gcRootsBuckets.begin(); iter != gcRootsBuckets.end(); iter++) {
        gcRootsSize = iter->second;
        StaticRootArray* array = iter->first;
        for (USize i = 0; i < gcRootsSize; i++) {
            NativeSlot* root = array->content[i];
            // make sure to visit each static root only once time.
            if (!visitedSet.insert(root).second) {
                continue;
            }
            visitor(*root);
        }
    }
}

void ExportRootTable::VisitGCRoots(const NativeSlotVisitor& visitor)
{
    std::lock_guard<std::mutex> lock(tableMutex);
    for (auto &rootInfo : exportRoots) {
        visitor(rootInfo.exportObj);
    }
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void TracingCollector::VisitStackRoots(const RootVisitor& visitor, RegSlotsMap& regSlotsMap, const FrameInfo& frame,
                                       Mutator& mutator)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    // HotSpot frame::oops_do_internal (frame.cpp:1166-1177) dispatches only
    // frames with managed metadata to oop-map scanning. Native callbacks
    // expose managed references through handles, not a Cangjie stack map.
#ifdef __APPLE__
    if (MFuncDesc::GetFuncDesc(frame.mFrame.GetFA()) == nullptr) {
#else
    if (MFuncDesc::GetFuncDesc(startIP) == nullptr) {
#endif
        return;
    }
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress);
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    DLOG(ENUM, "visit frame 0x%zx-@0x%zx, fp 0x%zx", startIP, frameIP, frameAddress);
    auto gcInfo = GCInfoNode::BuildNodeForTrace(startIP, frameIP, frame.mFrame.GetFA());
    auto slotDebugFunc = [&gcInfo](SlotBias off, BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            gcInfo.InsertSlotRoots<true>(off, root);
        } else {
            gcInfo.InsertSlotRoots<false>(off, root);
        }
    };
    auto regDebugFunc = [&gcInfo](RegisterNum i, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            gcInfo.InsertRegRoot<true>(i, root);
        } else {
            gcInfo.InsertRegRoot<false>(i, root);
        }
    };
#else
    SlotDebugVisitor slotDebugFunc = nullptr;
    RegDebugVisitor regDebugFunc = nullptr;
#endif

    // introot: use HeapReferenceMap so base/derived pairs are available. RootMap only
    // carries reg/slot roots and silently drops derived (RawArray+8 held across safepoint).
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(false);
    RootVisitor slotVisitor = visitor;
    RootVisitor regVisitor = visitor;

    if (heapMap.IsValid()) {
        auto derived = Mutator::MakeDerivedRootVisitor(visitor);
        heapMap.VisitDerivedPtr(derived, nullptr, regSlotsMap);
        heapMap.VisitSlotRoots(slotVisitor, slotDebugFunc);
        if (!heapMap.VisitRegRoots(regVisitor, regDebugFunc, regSlotsMap)) {
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
            mutator.PushFrameInfoForTrace(gcInfo);
#endif
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
    } else {
        RecordRootMapMiss(builder.GetInvalidReason(), frame, startIP, frameIP, mutator);
    }
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    mutator.PushFrameInfoForTrace(gcInfo);
#endif
    heapMap.RecordCalleeSaved(regSlotsMap);

}


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void TracingCollector::VisitHeapReferencesOnStack(const RootVisitor& rootVisitor,
                                                  const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                                  const FrameInfo& frame, Mutator& mutator, bool young)
{
    VisitHeapReferencesOnStack(rootVisitor, rootVisitor, derivedPtrVisitor, regSlotsMap, frame, mutator, young);
}

void TracingCollector::VisitHeapReferencesOnStack(const RootVisitor& regRootVisitor,
                                                  const RootVisitor& slotRootVisitor,
                                                  const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                                  const FrameInfo& frame, Mutator& mutator, bool young)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    // HotSpot frame::oops_do_internal (frame.cpp:1166-1177) dispatches only
    // frames with managed metadata to oop-map scanning. Native callbacks
    // expose managed references through handles, not a Cangjie stack map.
#ifdef __APPLE__
    if (MFuncDesc::GetFuncDesc(frame.mFrame.GetFA()) == nullptr) {
#else
    if (MFuncDesc::GetFuncDesc(startIP) == nullptr) {
#endif
        return;
    }
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress);
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(false);
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    auto infoNode = GCInfoNodeForFix::BuildNodeForFix(startIP, frameIP, frame.mFrame.GetFA());
    auto slotDebugFunc = [&infoNode](SlotBias off, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            infoNode.InsertSlotRoots<true>(off, root);
        } else {
            infoNode.InsertSlotRoots<false>(off, root);
        }
    };
    auto regDebugFunc = [&infoNode](RegisterNum i, const BaseObject* root) {
        if (Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(root))) {
            infoNode.InsertRegRoot<true>(i, root);
        } else {
            infoNode.InsertRegRoot<false>(i, root);
        }
    };
    auto derivedPtrDebugFunc = [&infoNode](BasePtrType basePtr, DerivedPtrType derivedPtr) {
        infoNode.InsertDerivedPtrRef(basePtr, derivedPtr);
    };
#else
    RegDebugVisitor regDebugFunc = nullptr;
    SlotDebugVisitor slotDebugFunc = nullptr;
    DerivedPtrDebugVisitor derivedPtrDebugFunc = nullptr;
#endif
    DLOG(ENUM, "visit heap-ref 0x%zx-@0x%zx, fp 0x%zx", startIP, frameIP, frameAddress);
    if (heapMap.IsValid()) {
        heapMap.VisitDerivedPtr(derivedPtrVisitor, derivedPtrDebugFunc, regSlotsMap);
        if (!heapMap.VisitRegRoots(regRootVisitor, regDebugFunc, regSlotsMap, young)) {
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
            mutator.PushFrameInfoForFix(infoNode);
#endif
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
        heapMap.VisitSlotRoots(slotRootVisitor, slotDebugFunc, young);
    } else {
        RecordSkippedStackMap(builder.GetInvalidReason(), frame, startIP, frameIP);
    }
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    mutator.PushFrameInfoForFix(infoNode);
#endif
    heapMap.RecordCalleeSaved(regSlotsMap);
}

void TracingCollector::RecordStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubCalleeSaved(regSlotsMap, fp);
}

#ifdef __arm__
void TracingCollector::RecordC2NStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordC2NStubCalleeSaved(regSlotsMap, fp);
}

void TracingCollector::RecordExclusiveStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordExclusiveStubCalleeSaved(regSlotsMap, fp);
}
#endif

void TracingCollector::RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubAllRegister(regSlotsMap, fp);
}





void TracingCollector::MergeMutatorRoots(WorkStack& workStack)
{
    (void)workStack;
    (void)MutatorManager::Instance().HandshakeFlushMarkProducers(majorMarkDomain.get());
}

void TracingCollector::EnumAllExportRoots(RootSet &foreignRootsSet)
{
    VisitExportColoredRoots([&foreignRootsSet, this](NativeSlot& root) {

        EnumRefFieldRoot(root, foreignRootsSet);
    });
}
void TracingCollector::DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    ScopedEntryTrace trace("CJRT_GC_ENUM");
    EnumAllCommonRoots(GetWorkers(GCCycleGeneration::OLD), workStack);
    EnumAllExportRoots(foreignRootsSet);
}


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
void TracingCollector::VisitExportColoredRoots(const NativeSlotVisitor& visitor) const
{
    Heap::GetHeap().VisitAllExportRoots(visitor);
}

// Each physical root family is enumerated here, shared by mark and remap.
void TracingCollector::VisitStrongColoredRoots(const NativeSlotVisitor& visitor) const
{
    VisitStaticRoots(visitor);
    VisitFinalizerRoots(visitor);
}

void TracingCollector::VisitWeakColoredRoots(const NativeSlotVisitor& visitor) const
{
    collectorResources.GetFinalizerProcessor().VisitFinalizers(visitor);
    VisitExportColoredRoots(visitor);
}

void TracingCollector::VisitAllColoredRoots(const NativeSlotVisitor& visitor) const
{
    VisitStrongColoredRoots(visitor);
    VisitWeakColoredRoots(visitor);
}

void RootsIteratorStrongColored::Apply(const NativeSlotVisitor& visitor)
{
    if (!claimed.exchange(true, std::memory_order_relaxed)) {
        collector.VisitStrongColoredRoots(visitor);
    }
}

void RootsIteratorWeakColored::Apply(const NativeSlotVisitor& visitor)
{
    if (!claimed.exchange(true, std::memory_order_relaxed)) {
        collector.VisitWeakColoredRoots(visitor);
    }
}

// ZRootsIteratorAllColored::apply, zRootsIterator.cpp:194-198.
void RootsIteratorAllColored::Apply(const NativeSlotVisitor& visitor)
{
    strong.Apply(visitor);
    weak.Apply(visitor);
}

void TracingCollector::VisitStrongPlainRoots(
    const RootVisitor& visitor, const std::function<void(Mutator&)>& threadVisitor) const
{
    if (threadVisitor) {
        MutatorManager::Instance().VisitAllMutators(threadVisitor);
    }
    RootVisitor plainVisitor = visitor;
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&plainVisitor);
}

void TracingCollector::VisitStaticRoots(const NativeSlotVisitor& visitor) const
{
    Heap::GetHeap().VisitStaticRoots(visitor);
}

void TracingCollector::VisitFinalizerRoots(const NativeSlotVisitor& visitor) const
{
    collectorResources.GetFinalizerProcessor().VisitGCRoots(visitor);
}


} // namespace MapleRuntime

namespace MapleRuntime {





}
