
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
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"
#include "Mutator/Mutator.h"
#include "Sync/Sync.h"


namespace MapleRuntime {
HandleMark::HandleMark(Mutator& mutator) : mutator(mutator), mark(mutator.NativeFrameRootCount()) {}
HandleMark::~HandleMark() { mutator.PopNativeFrameRootsTo(mark); }

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
    for (auto iter = gcRootsBuckets.begin(); iter != gcRootsBuckets.end(); iter++) {
        U32 gcRootsSize = iter->second;
        StaticRootArray* array = iter->first;
        for (USize i = 0; i < gcRootsSize; i++) {
            visitor(*array->content[i]);
        }
    }
}

void ExportRootTable::VisitGCRoots(const NativeSlotVisitor& visitor)
{
    weakStorage.OopsDo(visitor);
}


void CopyCollector::Process(const RootVisitor& visitor, const DerivedPtrVisitor* derivedPtrVisitor,
                               RegSlotsMap& regSlotsMap, const FrameInfo& frame, Mutator& mutator)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
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
    SlotDebugVisitor slotDebugFunc = nullptr;
    RegDebugVisitor regDebugFunc = nullptr;
    DerivedPtrVisitor derived =
        derivedPtrVisitor != nullptr ? *derivedPtrVisitor : Mutator::MakeDerivedRootVisitor(visitor);
    if (heapMap.IsValid()) {
        heapMap.VisitDerivedPtr(derived, nullptr, regSlotsMap);
        heapMap.VisitSlotRoots(visitor, slotDebugFunc);
        if (!heapMap.VisitRegRoots(visitor, regDebugFunc, regSlotsMap)) {
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
    } else {
        RecordRootMapMiss(builder.GetInvalidReason(), frame, startIP, frameIP, mutator);
    }
    heapMap.RecordCalleeSaved(regSlotsMap);
}





void CopyCollector::RecordStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubCalleeSaved(regSlotsMap, fp);
}

#ifdef __arm__
void CopyCollector::RecordC2NStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordC2NStubCalleeSaved(regSlotsMap, fp);
}

void CopyCollector::RecordExclusiveStubCalleeSaved(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordExclusiveStubCalleeSaved(regSlotsMap, fp);
}
#endif

void CopyCollector::RecordStubAllRegister(RegSlotsMap& regSlotsMap, Uptr fp)
{
    RegRoot::RecordStubAllRegister(regSlotsMap, fp);
}









void CopyCollector::VisitExportColoredRoots(const NativeSlotVisitor& visitor) const
{
    Heap::GetHeap().VisitAllExportRoots(visitor);
}

OopStorage& CopyCollector::StrongRootStorage() const
{
    return collectorResources.GetFinalizerProcessor().StrongRootStorage();
}

OopStorage& CopyCollector::WeakFinalizerRootStorage() const
{
    return collectorResources.GetFinalizerProcessor().WeakRootStorage();
}

OopStorage& CopyCollector::SyncWeakRootStorage() const
{
    return SyncWeakOopStorage();
}

void CopyCollector::VisitStaticAdapterRoots(const NativeSlotVisitor& visitor) const
{
    VisitStaticRoots(visitor);
}

void CopyCollector::VisitStrongColoredRoots(const NativeSlotVisitor& visitor) const
{
    RootsIteratorStrongColored roots(*this);
    roots.Apply(visitor);
}

void CopyCollector::VisitWeakColoredRoots(const NativeSlotVisitor& visitor) const
{
    RootsIteratorWeakColored roots(*this);
    roots.Apply(visitor);
}

void CopyCollector::VisitAllColoredRoots(const NativeSlotVisitor& visitor) const
{
    RootsIteratorAllColored roots(*this);
    roots.Apply(visitor);
}

OopStorageSetIteratorStrong::OopStorageSetIteratorStrong(const CopyCollector& collector, unsigned workers,
                                                         ZGenerationIdOptional generation)
    : states{{{collector.StrongRootStorage(), workers}}}, generation(generation)
{
    (void)this->generation;
}

OopStorageSetIteratorWeak::OopStorageSetIteratorWeak(const CopyCollector& collector, unsigned workers,
                                                     ZGenerationIdOptional generation)
    : states{{{collector.WeakFinalizerRootStorage(), workers},
              {Heap::GetHeap().GetExportRootStorage(), workers},
              {collector.SyncWeakRootStorage(), workers}}}, generation(generation) {}

void OopStorageSetIteratorWeak::report_num_dead()
{
    numDead = 0;
    for (auto& state : states) {
        state.OopsDo([&](NativeSlot& slot) {
            if (is_null(slot.GetTargetObject())) {
                ++numDead;
            }
        });
    }
}

void OopStorageSetIteratorStrong::Apply(const NativeSlotVisitor& visitor)
{
    // oopStorageSetParState.inline.hpp:38: every worker enters every storage.
    for (auto& state : states) { state.OopsDo(visitor); }
}

void OopStorageSetIteratorWeak::Apply(const NativeSlotVisitor& visitor)
{
    for (auto& state : states) { state.OopsDo(visitor); }
}

void StaticRootsAdapterIterator::Apply(const NativeSlotVisitor& visitor)
{
    if (!claimed.exchange(true, std::memory_order_relaxed)) {
        collector.VisitStaticAdapterRoots(visitor);
    }
}

void RootsIteratorStrongColored::Apply(const NativeSlotVisitor& visitor)
{
    NativeSlotVisitor copy = visitor;
    strong.apply(&copy);
    statics.apply(&copy);
}

void RootsIteratorAllColored::Apply(const NativeSlotVisitor& visitor)
{
    NativeSlotVisitor copy = visitor;
    strong.apply(&copy);
    weak.apply(&copy);
    statics.apply(&copy);
}

JavaThreadsIterator::JavaThreadsIterator(ZGenerationIdOptional generation)
    : claimed(0), generation(generation)
{
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) { threads.push_back(&mutator); });
}

uint32_t JavaThreadsIterator::claim()
{
    return __atomic_fetch_add(&claimed, 1u, __ATOMIC_RELAXED);
}

void JavaThreadsIterator::Apply(const std::function<void(Mutator&)>& visitor)
{
    for (;;) {
        const uint32_t index = claim();
        if (index >= threads.size()) {
            return;
        }
        visitor(*threads[index]);
    }
}

void CopyCollector::VisitStrongPlainRoots(
    const RootVisitor& visitor, const std::function<void(Mutator&)>& threadVisitor) const
{
    if (threadVisitor) {
        MutatorManager::Instance().VisitAllMutators(threadVisitor);
    }
    RootVisitor plainVisitor = visitor;
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&plainVisitor);
}

void CopyCollector::VisitStaticRoots(const NativeSlotVisitor& visitor) const
{
    Heap::GetHeap().VisitStaticRoots(visitor);
}

void CopyCollector::VisitFinalizerRoots(const NativeSlotVisitor& visitor) const
{
    collectorResources.GetFinalizerProcessor().VisitGCRoots(visitor);
}



void CopyCollector::VisitStackRoots(const RootVisitor& visitor, RegSlotsMap& regSlotsMap, const FrameInfo& frame,
                                       Mutator& mutator)
{
    Process(visitor, nullptr, regSlotsMap, frame, mutator);
}

void CopyCollector::VisitHeapReferencesOnStack(const RootVisitor& rootVisitor,
                                                  const DerivedPtrVisitor& derivedPtrVisitor, RegSlotsMap& regSlotsMap,
                                                  const FrameInfo& frame, Mutator& mutator, bool young)
{
    VisitHeapReferencesOnStack(rootVisitor, rootVisitor, derivedPtrVisitor, regSlotsMap, frame, mutator, young);
}

void CopyCollector::VisitHeapReferencesOnStack(const RootVisitor& regRootVisitor,
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

void CopyCollector::MergeMutatorRoots(WorkStack& workStack)
{
    (void)workStack;
    (void)Heap::GetHeap().old().Mark().Flush();
}

void CopyCollector::EnumAllExportRoots(RootSet &foreignRootsSet)
{
    VisitExportColoredRoots([&foreignRootsSet, this](NativeSlot& root) {

        EnumRefFieldRoot(root, foreignRootsSet);
    });
}
void CopyCollector::DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet)
{
    ScopedEntryTrace trace("CJRT_GC_ENUM");
    EnumAllCommonRoots(GetWorkers(ZGenerationId::old));
    MergeMutatorRoots(workStack);
    EnumAllExportRoots(foreignRootsSet);
}


} // namespace MapleRuntime
