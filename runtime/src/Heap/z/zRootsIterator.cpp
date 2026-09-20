
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Common/SuspendibleThreadSet.h"
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

#ifdef __arm__

#endif

OopStorageSetIteratorStrong::OopStorageSetIteratorStrong(unsigned workers,
                                                         ZGenerationIdOptional generation)
    : states{{{Heap::GetHeap().GetFinalizerProcessor().StrongRootStorage(), workers}}}, generation(generation)
{
    (void)this->generation;
}

OopStorageSetIteratorWeak::OopStorageSetIteratorWeak(unsigned workers,
                                                     ZGenerationIdOptional generation)
    : states{{{Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers},
              {Heap::GetHeap().GetExportRootStorage(), workers},
              {SyncWeakOopStorage(), workers}}}, generation(generation) {}

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
        Heap::GetHeap().VisitStaticRoots(visitor);
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

void ZMark::VisitStrongPlainRoots(
    const RootVisitor& visitor, const std::function<void(Mutator&)>& threadVisitor)
{
    if (threadVisitor) {
        MutatorManager::Instance().VisitAllMutators(threadVisitor);
    }
    RootVisitor plainVisitor = visitor;
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&plainVisitor);
}

void ZMark::VisitStaticRoots(const NativeSlotVisitor& visitor)
{
    Heap::GetHeap().VisitStaticRoots(visitor);
}

void ZMark::MergeMutatorRoots(WorkStack& workStack)
{
    (void)workStack;
    (void)Heap::GetHeap().old().Mark().Flush();
}

void ZMark::EnumAllExportRoots(ValueRootList& exportOwners)
{
    Heap::GetHeap().VisitAllExportRoots([&exportOwners](NativeSlot& root) {

        EnumRefFieldRoot(root, exportOwners);
    });
}
void ZMark::DoEnumeration(WorkStack& workStack, ValueRootList& exportOwners)
{
    // ZGC zMark.cpp:939-942: keep the entire old root task inside the
    // suspendible set. Its barriers must not color young roots across the
    // young mark-start flip before the new mark domain is ready.
    SuspendibleThreadSetJoiner joiner;
    EnumAllCommonRoots((*Heap::GetHeap().GetZGeneration(ZGenerationId::old).Workers()));
    MergeMutatorRoots(workStack);
    EnumAllExportRoots(exportOwners);
}


} // namespace MapleRuntime
