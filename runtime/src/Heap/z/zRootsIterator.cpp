#include "LoaderManager.h"
#include "Heap/z/zRootsIterator.hpp"

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/z/zAccess.hpp"
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
Handle::Handle(Mutator* mutator, BaseObject* object)
    : slot(object == nullptr ? nullptr : mutator->AddNativeFrameRoot(object)) {}
BaseObject* Handle::operator()() const
{
    return slot == nullptr ? nullptr : to_object(safe(slot->LoadPlain()));
}

HandleMark::HandleMark(Mutator& mutator) : mutator(mutator), mark(mutator.NativeFrameRootCount()) {}
HandleMark::~HandleMark() { mutator.PopNativeFrameRootsTo(mark); }

// Fill gc roots entry to buckets


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
              {Heap::GetHeap().cross_vm().export_roots().RootStorage(), workers},
              {SyncWeakOopStorage(), workers}}}, generation(generation) {}

void OopStorageSetIteratorWeak::report_num_dead()
{
    for (auto& state : states) {
        state.storage()->report_num_dead(state.num_dead());
    }
}

void OopStorageSetIteratorStrong::Apply(const NativeSlotVisitor& visitor)
{
    // oopStorageSetParState.inline.hpp:38: every worker enters every storage.
    for (auto& state : states) { state.OopsDo(visitor); }
}

// oopStorageSetParState.inline.hpp:44-70. NativeSlotVisitor is this
// runtime's closure carrier; dead counting remains a per-worker closure.
class DeadCounterClosure {
public:
    explicit DeadCounterClosure(const NativeSlotVisitor* closure) : closure(closure) {}
    void do_oop(NativeSlot* slot)
    {
        (*closure)(*slot);
        if (NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(slot) == nullptr) {
            ++numDead;
        }
    }
    size_t num_dead() const { return numDead; }
private:
    const NativeSlotVisitor* closure;
    size_t numDead = 0;
};

void OopStorageSetIteratorWeak::Apply(const NativeSlotVisitor& visitor)
{
    // oopStorageSetParState.inline.hpp:76-91: count in the clearing pass,
    // then notify each owner once after every worker has finished.
    for (auto& state : states) {
        if (state.storage()->should_report_num_dead()) {
            DeadCounterClosure countingClosure(&visitor);
            state.OopsDo([&](NativeSlot& slot) { countingClosure.do_oop(&slot); });
            state.increment_num_dead(countingClosure.num_dead());
        } else {
            state.OopsDo(visitor);
        }
    }
}

void StaticRootsAdapterIterator::Apply(const NativeSlotVisitor& visitor)
{
    if (!claimed.exchange(true, std::memory_order_relaxed)) {
        LoaderManager::GetInstance()->VisitStaticRoots(visitor);
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
}

uint32_t JavaThreadsIterator::claim()
{
    return __atomic_fetch_add(&claimed, 1u, __ATOMIC_RELAXED);
}

void JavaThreadsIterator::Apply(const std::function<void(Mutator&)>& visitor)
{
    for (;;) {
        const uint32_t index = claim();
        if (index >= threads.length()) {
            return;
        }
        visitor(*threads.thread_at(index));
    }
}

void ZMark::VisitStrongPlainRoots(
    const RootVisitor& visitor, const std::function<void(Mutator&)>& threadVisitor)
{
    if (threadVisitor) {
        MutatorManager::Instance().VisitAllMutators(threadVisitor);
    }
    (void)visitor;
    Runtime::Current().GetConcurrencyModel().VisitGCRoots();
}

void ZMark::VisitStaticRoots(const NativeSlotVisitor& visitor)
{
    LoaderManager::GetInstance()->VisitStaticRoots(visitor);
}

void ZMark::MergeMutatorRoots(WorkStack& workStack)
{
    (void)workStack;
    (void)Heap::GetHeap().old().Mark().Flush();
}

void ZMark::DoEnumeration(WorkStack& workStack, ValueRootList& exportOwners)
{
    // ZGC zMark.cpp:939-942: keep the entire old root task inside the
    // suspendible set. Its barriers must not color young roots across the
    // young mark-start flip before the new mark domain is ready.
    SuspendibleThreadSetJoiner joiner;
    EnumAllCommonRoots((*(*ZGeneration::old()).Workers()), exportOwners);
    MergeMutatorRoots(workStack);
}


} // namespace MapleRuntime
