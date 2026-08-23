// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MArray.inline.h"

#include <algorithm>

#include "Base/MemUtils.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/Collector/GcStats.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
namespace {
LargeArrayInitTestHooks g_largeArrayInitTestHooks;
} // namespace

extern "C" MRT_EXPORT void CJ_MRT_SetLargeArrayInitTestHooks(const LargeArrayInitTestHooks* hooks)
{
    g_largeArrayInitTestHooks = hooks == nullptr ? LargeArrayInitTestHooks{} : *hooks;
}

extern "C" MRT_EXPORT MAddress CJ_MRT_TestAllocateArrayStorage(size_t size, AllocType allocType)
{
    if (g_largeArrayInitTestHooks.allocate != nullptr) {
        return g_largeArrayInitTestHooks.allocate(size, allocType);
    }
    return HeapManager::Allocate(size, allocType);
}

void NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite site, BaseObject* object)
{
    if (g_largeArrayInitTestHooks.onRootVisit != nullptr) {
        g_largeArrayInitTestHooks.onRootVisit(site, object);
    }
}
#endif

MArray* MArray::InitializeLargeRefArray(MAddress address, MSize arraySize, MIndex nElems,
                                        TypeInfo& arrayClass)
{
    // Publish a complete boundary before the first yield. The invisible-root
    // release store below makes these plain header writes visible to GC.
    MArray* array = reinterpret_cast<MArray*>(SetClassInfo(address, &arrayClass));
    array->SetLength(nElems);
    // ZObjArrayAllocator marks the published header so every ZIterator skips
    // the incomplete object array (zObjArrayAllocator.cpp:92-112,
    // zIterator.inline.hpp:56-70). The side root controls liveness; this header
    // bit independently controls heap iteration.
    array->SetInvisibleObject(true);

    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "large reference array initialization requires a mutator");
    mutator->PublishInvisibleRoot(array);
#if defined(MRT_GC_UNIT_TESTS)
    if (g_largeArrayInitTestHooks.onPublish != nullptr) {
        g_largeArrayInitTestHooks.onPublish(array);
    }
#endif

    const size_t contentOffset = GetContentOffset();
    CHECK_DETAIL(arraySize >= contentOffset, "large reference array size is smaller than its header");
    // Clear through the aligned object end, including tail padding. The allocator
    // deliberately leaves a reused extent dirty for this path.
    const size_t contentSize = static_cast<size_t>(arraySize) - contentOffset;
    size_t processed = 0;
    size_t segmentIndex = 0;
    size_t epoch = g_gcCount.load(std::memory_order_acquire);
    while (processed < contentSize) {
        MArray* current = static_cast<MArray*>(mutator->LoadInvisibleRoot());
        CHECK_DETAIL(current != nullptr, "large reference array lost its invisible root");
        const size_t segment = std::min(contentSize - processed,
                                        static_cast<size_t>(LARGE_REF_ARRAY_INIT_SEGMENT_SIZE));
        const MAddress start = reinterpret_cast<MAddress>(current->ConvertToCArray()) + processed;
        // RefField raw null is the all-zero word (RefField.h:427-433); unlike ZGC,
        // no epoch-coloured null fill is needed in this runtime.
        MemorySet(start, segment, 0, segment);

        {
            // Entering a saferegion is this runtime's mutator/GC handshake edge.
            // The root stays published throughout the whole interval.
            ScopedEnterSaferegion yield(true);
#if defined(MRT_GC_UNIT_TESTS)
            if (g_largeArrayInitTestHooks.onYield != nullptr) {
                g_largeArrayInitTestHooks.onYield(segmentIndex);
            }
#endif
        }

        const size_t observedEpoch = g_gcCount.load(std::memory_order_acquire);
        if (UNLIKELY(observedEpoch != epoch)) {
            // GC may have copied a version containing a block from before this
            // initializer's last write. Reacquire through the healed root and
            // rewrite every block in the new version.
            epoch = observedEpoch;
            processed = 0;
            segmentIndex = 0;
            continue;
        }
        processed += segment;
        ++segmentIndex;
    }

    MArray* complete = static_cast<MArray*>(mutator->WithdrawInvisibleRoot());
    complete->SetInvisibleObject(false);
#if defined(MRT_GC_UNIT_TESTS)
    if (g_largeArrayInitTestHooks.onWithdraw != nullptr) {
        g_largeArrayInitTestHooks.onWithdraw(complete);
    }
#endif
    return complete;
}

void MArray::ForEachRefFieldInRange(const RefFieldVisitor& visitor, MAddress fieldStart, MIndex fieldEnd) const
{
    if (IsInvisibleObject()) {
#if defined(MRT_GC_UNIT_TESTS)
        NoteLargeArrayInitRootVisit(LargeArrayRootVisitSite::ITERATOR_SKIP,
                                    const_cast<MArray*>(this));
#endif
        return;
    }
    TypeInfo* componentTi = GetComponentTypeInfo();
    MIndex size = fieldEnd - fieldStart;
    if (componentTi->IsStructType()) {
        GCTib gcTib = componentTi->GetGCTib();
        size_t elementSize = GetElementSize();
        CHECK(elementSize != 0);
        MIndex limit = size / elementSize;
        for (MIndex i = 0; i < limit; ++i) {
            gcTib.ForEachBitmapWord(fieldStart, visitor);
            fieldStart += elementSize;
        }
    } else if (componentTi->IsObjectType() || componentTi->IsArrayType() || componentTi->IsInterface()) {
        HeapSlot<false>* arrayContent = &HeapSlotAt<false>(fieldStart);
        MIndex upLimit = size / sizeof(RefField<>);
        for (MIndex i = 0; i < upLimit; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", this);
    }
}
} // namespace MapleRuntime
