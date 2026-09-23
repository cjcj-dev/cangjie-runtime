// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ObjectModel/MArray.inline.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "Heap/z/zIterator.inline.hpp"

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
MArray* ZObjArrayAllocator::initialize()
{
    // Publish a complete boundary before the first yield. The invisible-root
    // release store below makes these plain header writes visible to GC.
    MArray* array = reinterpret_cast<MArray*>(BaseObject::SetClassInfo(address, &arrayClass));
    array->SetLength(nElems);
    // ZObjArrayAllocator marks the published header so every ZIterator skips
    // the incomplete object array (zObjArrayAllocator.cpp:92-112,
    // zIterator.inline.hpp:56-70). The side root controls liveness; this header
    // bit independently controls heap iteration.
    array->SetInvisibleObject(true);

    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "large array initialization requires a mutator");
    mutator->PublishInvisibleRoot(array);

    const size_t contentOffset = MArray::GetContentOffset();
    CHECK_DETAIL(arraySize >= contentOffset, "large array size is smaller than its header");
    // Clear through the aligned object end, including tail padding. The allocator
    // deliberately leaves a reused extent dirty for this path.
    const size_t contentSize = MRT_ALIGN(static_cast<size_t>(arraySize), sizeof(uintptr_t)) - contentOffset;
    const bool isRefArray = arrayClass.GetComponentTypeInfo()->IsRef();
    // zObjArrayAllocator.cpp:132-141: a safepoint may change either
    // generation sequence before its collection has completed.
    Heap& heap = Heap::GetHeap();
    const uint64_t youngSequenceBefore = heap.GetCycleSnapshot(ZGenerationId::young).sequence;
    const uint64_t oldSequenceBefore = heap.GetCycleSnapshot(ZGenerationId::old).sequence;
    const uintptr_t colorBefore = ::g_cjStoreGoodMask;
    bool seenGcSafepoint = false;
#if defined(MRT_GC_UNIT_TESTS)
    // Existing registered managed-segmented test mode. It only requests a real
    // collection; allocation, root publication and clearing stay product-owned.
    const char* gcMode = std::getenv("MRT_GC_UNIT_MANAGED_SEGMENTED");
    const bool requestYoung = gcMode != nullptr && std::strcmp(gcMode, "young") == 0;
    const bool requestOld = gcMode != nullptr && (std::strcmp(gcMode, "full") == 0 || std::strcmp(gcMode, "full2") == 0);
    const size_t gcRequests = gcMode != nullptr && std::strcmp(gcMode, "full2") == 0 ? 2 : 1;
    size_t requestedGc = 0;
    size_t passes = 0;
#endif
    // ZObjArrayAllocator::initialize (zObjArrayAllocator.cpp:140-200):
    // only the first pass can request a restart. Primitive payloads never do.
    auto initializeMemory = [&]() {
#if defined(MRT_GC_UNIT_TESTS)
        ++passes;
#endif
        size_t segmentIndex = 0;
        for (size_t processed = 0; processed < contentSize; ++segmentIndex) {
            MArray* current = static_cast<MArray*>(mutator->LoadInvisibleRoot());
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

            {
                // Entering a saferegion is this runtime's mutator/GC handshake edge.
                // The root stays published throughout the whole interval.
                yield_for_safepoint();
#if defined(MRT_GC_UNIT_TESTS)
                if (requestedGc < gcRequests && (requestYoung || requestOld)) {
                    ++requestedGc;
                    ScopedEnterSaferegion testYield(true);
                    MArray* observed = static_cast<MArray*>(mutator->LoadInvisibleRoot());
                    CHECK(observed != nullptr && observed->IsInvisibleObject());
                    size_t fields = 0;
                    auto visitor = [&](RefField<>&) { ++fields; };
                    ZBasicOopIterateClosure<decltype(visitor)> closure(visitor);
                    ZIterator::oop_iterate_safe(observed, &closure);
                    CHECK_DETAIL(fields == 0, "incomplete array must not expose reference fields");
                    const ZGenerationId id = requestYoung ? ZGenerationId::young : ZGenerationId::old;
                    const uint64_t before = heap.GetCycleSnapshot(id).sequence;
                    heap.RequestGC(requestYoung ? GC_REASON_YOUNG : GC_REASON_FORCE, false);
                    const uint64_t after = heap.GetCycleSnapshot(id).sequence;
                    observed = static_cast<MArray*>(mutator->LoadInvisibleRoot());
                    const bool valid = observed != nullptr && observed->IsInvisibleObject() &&
                                       observed->GetLength() == nElems;
                    std::fprintf(stderr, "SEGMENTED_GC_WINDOW mode=%s before=%llu after=%llu root=%d fields=%zu\n",
                                 gcMode, (unsigned long long)before, (unsigned long long)after, valid, fields);
                    CHECK_DETAIL(after != before && valid, "collection must complete inside the initialization window");
                }
#endif

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
        // Refill the whole array with both remembered bits after a GC safepoint.
        // ZGC zObjArrayAllocator.cpp:179-182.
        const bool complete = initializeMemory();
        CHECK_DETAIL(complete, "array initialization must complete on the second pass");
    }

#if defined(MRT_GC_UNIT_TESTS)
    if (requestYoung || requestOld) {
        const size_t expectedPasses = isRefArray ? 2 : 1;
        std::fprintf(stderr, "SEGMENTED_PASSES_TARGET requested=%zu passes=%zu expected=%zu\n",
                     requestedGc, passes, expectedPasses);
        CHECK_DETAIL(requestedGc == gcRequests && passes == expectedPasses,
                     "array restart count must be bounded independently of collection count");
    }
#endif
    MArray* complete = static_cast<MArray*>(mutator->WithdrawInvisibleRoot());
    complete->SetInvisibleObject(false);
    return complete;
}


}
