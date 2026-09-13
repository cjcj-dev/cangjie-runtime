// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Collector/HeapIterator.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
void HeapIterator::Push(BaseObject* object)
{
    if (object != nullptr && visited.insert(object).second) {
        stack.push_back(object);
    }
}

void HeapIterator::Fields(BaseObject* object, bool visitReferents, const FieldVisitor& visitor)
{
    // The Cangjie WeakRef payload's first field is the referent. It must itself
    // be visited, rather than following only the referent's outgoing fields.
    const uintptr_t referent = reinterpret_cast<uintptr_t>(object) + TYPEINFO_PTR_SIZE;
    if (object->IsWeakRef() && visitReferents) { visitor(object, HeapSlotAt<>(referent)); }
    object->ForEachRefField([&](RefField<>& field) {
        if (object->IsWeakRef() && reinterpret_cast<uintptr_t>(&field) == referent) {
            return;
        }
        visitor(object, field);
    });
}

void HeapIterator::Iterate(const ObjectVisitor& objectVisitor, const FieldVisitor& fieldVisitor)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked());
    visited.clear();
    stack.clear();
    auto& collector = static_cast<TracingCollector&>(Heap::GetHeap().GetCollector());
    NativeSlotVisitor colored = [&](NativeSlot& root) {
        if (fieldVisitor) { fieldVisitor(nullptr, root); }
        // Strong loads only remap/heal: no keepalive marking during inspection.
        Push(Heap::GetBarrier().ReadStaticRef(root));
    };
    collector.VisitStrongColoredRoots(colored);
    if (visitWeaks) { collector.VisitWeakColoredRoots(colored); }
    RootVisitor plain = [&](ObjectRef& root) { Push(Heap::GetBarrier().ReadPlainRoot(root)); };
    collector.VisitStrongPlainRoots(plain, [&](Mutator& mutator) {
        // Complete root processing for graph traversal, after ZVerify's raw-root
        // checks. VisitMutatorRoots enumerates the complete stack and non-frame roots.
        mutator.VisitMutatorRoots(plain);
    });
    while (!stack.empty()) {
        BaseObject* object = stack.back();
        stack.pop_back();
        DCHECK(object->IsValidObject());
        objectVisitor(object);
        Fields(object, visitWeaks, [&](BaseObject* base, RefField<>& field) {
            if (fieldVisitor) { fieldVisitor(base, field); }
            Push(Heap::GetBarrier().ReadReference(base, field));
        });
    }
}
} // namespace MapleRuntime
