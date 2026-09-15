// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include <algorithm>

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
    // zHeapIterator.cpp:433: graph verification uses the unsafe iterator.
    auto fields = [&](RefField<>& field) {
        if (object->IsWeakRef() && reinterpret_cast<uintptr_t>(&field) == referent) {
            return;
        }
        visitor(object, field);
    };
    ZBasicOopIterateClosure<decltype(fields)> closure(fields);
    ZIterator::oop_iterate(object, &closure);
}

void HeapIterator::Follow(BaseObject* object, const FieldVisitor& visitor)
{
    // zHeapIterator.cpp:463-469: array work has its own range consumer.
    if (object->GetTypeInfo()->IsRawArray() && object->GetComponentTypeInfo()->IsRef()) {
        FollowArray(static_cast<MArray*>(object));
    } else {
        Fields(object, visitWeaks, visitor);
    }
}

void HeapIterator::FollowArray(MArray* object)
{
    // zHeapIterator.cpp:436-443. TypeInfo is not a managed Klass object;
    // the VM adapter has no metadata oop edge to enqueue here.
    arrayStack.push_back({object, 0});
}

void HeapIterator::FollowArrayChunk(const ObjArrayTask& array, const FieldVisitor& visitor)
{
    // zHeapIterator.cpp:445-459 / gc_globals.hpp:253.
    constexpr MIndex strideLimit = 2048;
    const MIndex length = array.object->GetLength();
    const MIndex start = array.index;
    const MIndex end = start + std::min(length - start, strideLimit);
    if (end < length) {
        arrayStack.push_back({array.object, end});
    }
    RefFieldVisitor fields = [&](RefField<>& field) { visitor(array.object, field); };
    ZBasicOopIterateClosure<RefFieldVisitor> closure(fields);
    ZIterator::oop_iterate_elements_range(array.object, &closure, start, end);
}

void HeapIterator::Iterate(const ObjectVisitor& objectVisitor, const EdgeVisitor& fieldVisitor)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked());
    visited.clear();
    stack.clear();
    arrayStack.clear();
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
        }, [](ObjectRef&) {
            // zHeapIterator.cpp:386 uses thread oops, not ZThreadLocalData's
            // separate invisible root (zObjArrayAllocator.cpp:107-112).
        });
    });
    FieldVisitor followField = [&](BaseObject* base, RefField<>& field) {
        if (fieldVisitor) { fieldVisitor(base, &field, raw(field.GetFieldValue())); }
        Push(Heap::GetBarrier().ReadReference(base, field), objectVisitor);
    };
    // zHeapIterator.cpp:480-493: drain object work, then one array chunk,
    // returning to newly discovered objects before consuming another chunk.
    do {
        while (!stack.empty()) {
            BaseObject* object = stack.back();
            stack.pop_back();
            DCHECK(object->IsValidObject());
            if (!forVerify) { objectVisitor(object); }
            Follow(object, followField);
        }
        if (!arrayStack.empty()) {
            const ObjArrayTask array = arrayStack.back();
            arrayStack.pop_back();
            FollowArrayChunk(array, followField);
        }
    } while (!stack.empty() || !arrayStack.empty());
}
} // namespace MapleRuntime

namespace MapleRuntime {
HeapIterator::HeapIterator(bool visitWeaks, bool forVerify ) : visitWeaks(visitWeaks), forVerify(forVerify) {}
}
