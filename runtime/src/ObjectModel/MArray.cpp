// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MArray.inline.h"

#include <algorithm>

#include "Base/MemUtils.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {

MArray* MArray::InitializeLargeArray(MAddress address, MSize arraySize, MIndex nElems,
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
    CHECK_DETAIL(mutator != nullptr, "large array initialization requires a mutator");
    mutator->PublishInvisibleRoot(array);

    const size_t contentOffset = GetContentOffset();
    CHECK_DETAIL(arraySize >= contentOffset, "large array size is smaller than its header");
    // Clear through the aligned object end, including tail padding. The allocator
    // deliberately leaves a reused extent dirty for this path.
    const size_t contentSize = static_cast<size_t>(arraySize) - contentOffset;
    const bool isRefArray = arrayClass.GetComponentTypeInfo()->IsRef();
    // zObjArrayAllocator.cpp:132-141: a safepoint may change either
    // generation sequence before its collection has completed.
    Heap& heap = Heap::GetHeap();
    const uint64_t youngSequenceBefore = heap.GetCycleSnapshot(ZGenerationId::young).sequence;
    const uint64_t oldSequenceBefore = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const uintptr_t colorBefore = ::g_cjStoreGoodMask;
    bool seenGcSafepoint = false;
    // ZObjArrayAllocator::initialize (zObjArrayAllocator.cpp:140-200):
    // only the first pass can request a restart. Primitive payloads never do.
    auto initializeMemory = [&]() {
        size_t segmentIndex = 0;
        for (size_t processed = 0; processed < contentSize; ++segmentIndex) {
            MArray* current = static_cast<MArray*>(mutator->LoadInvisibleRoot());
            CHECK_DETAIL(current != nullptr, "large array lost its invisible root");
            const size_t segment = std::min(contentSize - processed,
                                            static_cast<size_t>(LARGE_ARRAY_INIT_SEGMENT_SIZE));
            const MAddress start = reinterpret_cast<MAddress>(current->ConvertToCArray()) + processed;
            // RefField raw null is the all-zero word (RefField.h:427-433); unlike ZGC,
            // no epoch-coloured null fill is needed in this runtime.
            MemorySet(start, segment, 0, segment);

            {
                // Entering a saferegion is this runtime's mutator/GC handshake edge.
                // The root stays published throughout the whole interval.
                ScopedEnterSaferegion yield(true);
            }

            if (isRefArray && !seenGcSafepoint &&
                (heap.GetCycleSnapshot(ZGenerationId::young).sequence != youngSequenceBefore ||
                 heap.GetCycleSnapshot(ZGenerationId::old).sequence != oldSequenceBefore ||
                 static_cast<uintptr_t>(::g_cjStoreGoodMask) != colorBefore)) {
                seenGcSafepoint = true;
                return false;
            }
            processed += segment;
        }
        return true;
    };

    if (!initializeMemory()) {
        // Raw zero remains a legal null across every color flip in this runtime.
        // The second pass therefore needs no remembered-bit fill, and cannot
        // restart. Each segment still reloads the GC-healed invisible root.
        const bool complete = initializeMemory();
        CHECK_DETAIL(complete, "array initialization must complete on the second pass");
    }

    MArray* complete = static_cast<MArray*>(mutator->WithdrawInvisibleRoot());
    complete->SetInvisibleObject(false);
    return complete;
}

void MArray::ForEachRefFieldInRange(const RefFieldVisitor& visitor, MAddress fieldStart, MIndex fieldEnd) const
{
    // VM layout adapter. ZIterator's range entry requires visible ref arrays;
    // this byte-range form also supports Cangjie's inline struct-array copies.
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
