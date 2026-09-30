// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ObjectModel/MArray.inline.h"

#include <algorithm>

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zUtils.hpp"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"

#include "Heap/z/zObjArrayAllocator.hpp"
namespace MapleRuntime {
void ZObjArrayAllocator::yield_for_safepoint() const
{
    ScopedEnterSaferegion yield(true);
}
static bool is_ref_containing_flat_array(TypeInfo* arrayClass)
{
    TypeInfo* component = arrayClass->GetComponentTypeInfo();
    return !component->IsRef() && component->HasRefField();
}

MArray* ZObjArrayAllocator::initialize(MAddress address) const
{
    // ZGC zObjArrayAllocator.cpp:46-77: all specialization decisions are here.
    if (!doZero) {
        return ObjArrayAllocator::initialize(address);
    }
    if (arraySize <= MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE) {
        return ObjArrayAllocator::initialize(address);
    }
    if (is_ref_containing_flat_array(&arrayClass)) {
        return ObjArrayAllocator::initialize(address);
    }
    // Our compact header combines klass and state. Publish both in one release
    // store, like ZGC's compact-header branch (zObjArrayAllocator.cpp:94-95).
    MArray* array = reinterpret_cast<MArray*>(BaseObject::SetClassInfo(address, &arrayClass, true));
    array->SetLength(nElems);

    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "large array initialization requires a mutator");
    zaddress_unsafe mem = to_zaddress_unsafe(reinterpret_cast<uintptr_t>(array));
    mutator->GetGCData().set_invisible_root(&mem);

    const size_t contentOffset = MArray::GetContentOffset();
    CHECK_DETAIL(arraySize >= contentOffset, "large array size is smaller than its header");
    // Clear through the aligned object end, including tail padding. The allocator
    // deliberately leaves a reused extent dirty for this path.
    const size_t contentSize = MRT_ALIGN(static_cast<size_t>(arraySize), sizeof(uintptr_t)) - contentOffset;
    const bool isRefArray = arrayClass.GetComponentTypeInfo()->IsRef();
    // zObjArrayAllocator.cpp:132-141: a safepoint may change either
    // generation sequence before its collection has completed.
    const uint64_t youngSequenceBefore = (*ZGeneration::young()).seqnum();
    const uint64_t oldSequenceBefore = (*ZGeneration::old()).seqnum();
    const uintptr_t colorBefore = ::g_cjStoreGoodMask;
    bool seenGcSafepoint = false;
    // ZObjArrayAllocator::initialize (zObjArrayAllocator.cpp:140-200):
    // only the first pass can request a restart. Primitive payloads never do.
    auto initializeMemory = [&]() {
        for (size_t processed = 0; processed < contentSize;) {
            MArray* current = static_cast<MArray*>(to_object(safe(mem)));
            CHECK_DETAIL(current != nullptr, "large array lost its invisible root");
            const size_t segment = std::min(contentSize - processed,
                                            static_cast<size_t>(MArray::LARGE_ARRAY_INIT_SEGMENT_SIZE));
            const MAddress start = reinterpret_cast<MAddress>(current->ConvertToCArray()) + processed;
            // Invisible roots are hidden from marking. After a GC safepoint,
            // both remembered bits force subsequent stores through the barrier.
            // ZGC zObjArrayAllocator.cpp:146-161.
            const uintptr_t coloredNull = seenGcSafepoint ? (::g_cjStoreGoodMask | ZPointerRememberedMask)
                                                         : ::g_cjStoreGoodMask;
            const uintptr_t fillValue = isRefArray ? coloredNull : 0;
            ZUtils::fill(reinterpret_cast<uintptr_t*>(start), segment / sizeof(uintptr_t), fillValue);

            yield_for_safepoint();

            if (isRefArray && !seenGcSafepoint &&
                ((*ZGeneration::young()).seqnum() != youngSequenceBefore ||
                 (*ZGeneration::old()).seqnum() != oldSequenceBefore ||
                 static_cast<uintptr_t>(::g_cjStoreGoodMask) != colorBefore)) {
                seenGcSafepoint = true;
                return false;
            }
            processed += segment;
        }
        return true;
    };

    if (!initializeMemory()) {
        // Refill the whole array with both remembered bits after a GC safepoint.
        // ZGC zObjArrayAllocator.cpp:179-182.
        const bool complete = initializeMemory();
        CHECK_DETAIL(complete, "array initialization must complete on the second pass");
    }

    mutator->GetGCData().clear_invisible_root();
    MArray* complete = static_cast<MArray*>(to_object(safe(mem)));
    complete->SetInvisibleObject(false);
    return complete;
}


}
