// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
void HeapIterator::Push(BaseObject* object, const ObjectVisitor& objectVisitor)
{
    if (object != nullptr && visited.insert(object).second) {
        // zHeapIterator.cpp:329-339,421-428: verify at discovery so the
        // field visitor still describes the edge which reached this object.
        if (forVerify) { objectVisitor(object); }
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

void HeapIterator::Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked());
    visited.clear();
    stack.clear();
    auto& collector = static_cast<TracingCollector&>(Heap::GetHeap().GetCollector());
    NativeSlotVisitor colored = [&](NativeSlot& root) {
        if (fieldVisitor) { fieldVisitor(nullptr, &root, raw(root.GetFieldValue())); }
        // Strong loads only remap/heal: no keepalive marking during inspection.
        Push(Heap::GetBarrier().ReadStaticRef(root), objectVisitor);
    };
    collector.VisitStrongColoredRoots(colored);
    if (visitWeaks) { collector.VisitWeakColoredRoots(colored); }
    RootVisitor plain = [&](ObjectRef& root) {
        if (fieldVisitor) { fieldVisitor(nullptr, &root, raw(root.LoadPlain())); }
        Push(Heap::GetBarrier().ReadPlainRoot(root), objectVisitor);
    };
    collector.VisitStrongPlainRoots(plain, [&](Mutator& mutator) {
        // Complete root processing for graph traversal, after ZVerify's raw-root
        // checks. VisitMutatorRoots enumerates the complete stack and non-frame roots.
        mutator.VisitMutatorRoots([&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, plain);
        });
    });
    while (!stack.empty()) {
        BaseObject* object = stack.back();
        stack.pop_back();
        DCHECK(object->IsValidObject());
        // Inspection callbacks run after root enumeration, avoiding lock
        // ordering between root registries and the inspecting consumer.
        if (!forVerify) { objectVisitor(object); }
        Fields(object, visitWeaks, [&](BaseObject* base, RefField<>& field) {
            if (fieldVisitor) { fieldVisitor(base, &field, raw(field.GetFieldValue())); }
            Push(Heap::GetBarrier().ReadReference(base, field), objectVisitor);
        });
    }
}
} // namespace MapleRuntime

namespace MapleRuntime {
HeapIterator::HeapIterator(bool visitWeaks, bool forVerify ) : visitWeaks(visitWeaks), forVerify(forVerify) {}
}
