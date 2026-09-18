// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MArray.inline.h"

#include <algorithm>
#if defined(MRT_GC_UNIT_TESTS)
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "Heap/z/zIterator.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zHeap.hpp"
#endif

#include "Base/MemUtils.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
namespace {
LargeArrayInitTestHooks g_largeArrayInitTestHooks;

enum class ManagedSegmentedGc : uint8_t {
    NONE,
    YOUNG,
    FULL,
};

std::atomic<bool> g_managedSegmentedActive { false };
std::atomic<uint32_t> g_managedSegmentedVisitSites { 0 };

ManagedSegmentedGc GetManagedSegmentedGc()
{
    const char* value = std::getenv("MRT_GC_UNIT_MANAGED_SEGMENTED");
    if (value != nullptr && std::strcmp(value, "young") == 0) {
        return ManagedSegmentedGc::YOUNG;
    }
    if (value != nullptr && std::strcmp(value, "full") == 0) {
        return ManagedSegmentedGc::FULL;
    }
    return ManagedSegmentedGc::NONE;
}

uint32_t VisitBit(LargeArrayRootVisitSite site)
{
    return uint32_t { 1 } << static_cast<unsigned>(site);
}
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
    if (g_managedSegmentedActive.load(std::memory_order_acquire) && object != nullptr &&
        object->IsInvisibleObject()) {
        g_managedSegmentedVisitSites.fetch_or(VisitBit(site), std::memory_order_acq_rel);
    }
    if (g_largeArrayInitTestHooks.onRootVisit != nullptr) {
        g_largeArrayInitTestHooks.onRootVisit(site, object);
    }
}

void NoteLargeArrayInitRootPhase(LargeArrayRootPhase phase, Mutator* mutator, bool watermarkDone)
{
    if (g_largeArrayInitTestHooks.onRootPhase != nullptr) {
        g_largeArrayInitTestHooks.onRootPhase(phase, mutator, watermarkDone);
    }
}

#endif

MArray* MArray::InitializeLargeArray(MAddress address, MSize arraySize, MIndex nElems,
                                        TypeInfo& arrayClass)
{
#if defined(MRT_GC_UNIT_TESTS)
    const ManagedSegmentedGc managedTestGc = GetManagedSegmentedGc();
    const bool managedTest = managedTestGc != ManagedSegmentedGc::NONE &&
        arrayClass.GetComponentTypeInfo()->IsRef();
    bool managedTestRequested = false;
    std::array<size_t, 4> managedIteratorVisits {};
    if (managedTest) {
        bool expectedInactive = false;
        CHECK_DETAIL(g_managedSegmentedActive.compare_exchange_strong(
                         expectedInactive, true, std::memory_order_acq_rel),
                     "managed segmented-array test permits one active initializer");
        g_managedSegmentedVisitSites.store(0, std::memory_order_release);
    }
#endif
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
#if defined(MRT_GC_UNIT_TESTS)
    if (g_largeArrayInitTestHooks.onPublish != nullptr) {
        g_largeArrayInitTestHooks.onPublish(array);
    }
#endif

    const size_t contentOffset = GetContentOffset();
    CHECK_DETAIL(arraySize >= contentOffset, "large array size is smaller than its header");
    // Clear through the aligned object end, including tail padding. The allocator
    // deliberately leaves a reused extent dirty for this path.
    const size_t contentSize = static_cast<size_t>(arraySize) - contentOffset;
    const bool isRefArray = arrayClass.GetComponentTypeInfo()->IsRef();
    // zObjArrayAllocator.cpp:132-141: a safepoint may change either
    // generation sequence before its collection has completed.
    Collector& collector = Heap::GetHeap().GetCollector();
    const uint64_t youngSequenceBefore = collector.GetCycleSnapshot(ZGenerationId::young).sequence;
    const uint64_t oldSequenceBefore = collector.GetCycleSnapshot(ZGenerationId::old).sequence;
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
#if defined(MRT_GC_UNIT_TESTS)
                if (g_largeArrayInitTestHooks.onYield != nullptr) {
                    g_largeArrayInitTestHooks.onYield(segmentIndex);
                }
                if (managedTest && !managedTestRequested && segmentIndex == 0) {
                    managedTestRequested = true;
                    CHECK_DETAIL(mutator->IsManagedContext(),
                                 "language-level segmented-array test must retain managed context");
                    // Iterator test, not GC root visitation. Only safe entries
                    // may consume an incomplete array (zIterator.inline.hpp:64).
                    // Range/mark are tested after initialization, not here.
                    MArray* observed = static_cast<MArray*>(mutator->LoadInvisibleRoot());
                    RefFieldVisitor first = [&](RefField<>&) { ++managedIteratorVisits[0]; };
                    RefFieldVisitor second = [&](RefField<>&) { ++managedIteratorVisits[1]; };
                    ZBasicOopIterateClosure<RefFieldVisitor> firstClosure(first);
                    ZBasicOopIterateClosure<RefFieldVisitor> secondClosure(second);
                    ZIterator::oop_iterate_safe(observed, &firstClosure);
                    ZIterator::oop_iterate_safe(observed, observed->GetTypeInfo(), &secondClosure);
                    ZIterator::basic_oop_iterate_safe(observed,
                        RefFieldVisitor([&](RefField<>&) { ++managedIteratorVisits[2]; }));
                    ZIterator::basic_oop_iterate_safe(observed, observed->GetTypeInfo(),
                        RefFieldVisitor([&](RefField<>&) { ++managedIteratorVisits[3]; }));
                    std::fprintf(stderr, "[SEGMENTED_MANAGED_ITERATORS] safe=%zu safe_klass=%zu basic=%zu basic_klass=%zu\n",
                                 managedIteratorVisits[0], managedIteratorVisits[1],
                                 managedIteratorVisits[2], managedIteratorVisits[3]);
                    const ZGenerationId generation = managedTestGc == ManagedSegmentedGc::YOUNG
                        ? ZGenerationId::young : ZGenerationId::old;
                    const uint64_t sequenceBefore = collector.GetCycleSnapshot(generation).sequence;
                    if (managedTestGc == ManagedSegmentedGc::YOUNG) {
                        Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
                    } else {
                        Heap::GetHeap().GetCollector().RequestGC(GC_REASON_FORCE, false);
                    }
                    CHECK_DETAIL(collector.GetCycleSnapshot(generation).sequence != sequenceBefore,
                                 "language-level segmented-array GC did not advance the epoch");
                }
#endif
            }

            if (isRefArray && !seenGcSafepoint &&
                (collector.GetCycleSnapshot(ZGenerationId::young).sequence != youngSequenceBefore ||
                 collector.GetCycleSnapshot(ZGenerationId::old).sequence != oldSequenceBefore ||
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
#if defined(MRT_GC_UNIT_TESTS)
    if (managedTest) {
        // zMark.cpp:704-708 / zStackWatermark.cpp:171-173: the completed
        // watermark owns this root, as in native RequiredPhaseRootVisits.
        // No second mutator walk or minor-relocate stack consumer is required.
        const uint32_t required = VisitBit(LargeArrayRootVisitSite::STACK_WATERMARK_MANAGED) |
            VisitBit(LargeArrayRootVisitSite::ITERATOR_SKIP);
        const uint32_t sites = g_managedSegmentedVisitSites.load(std::memory_order_acquire);
        const uint32_t forbidden = VisitBit(LargeArrayRootVisitSite::MUTATOR_STACK_NATIVE) |
            VisitBit(LargeArrayRootVisitSite::STACK_WATERMARK_NATIVE);
        CHECK_DETAIL((sites & required) == required,
                     "language-level segmented-array GC missed managed root consumer: required=%#x actual=%#x",
                     required, sites);
        CHECK_DETAIL((sites & forbidden) == 0,
                     "language-level segmented-array GC entered native root consumer: forbidden=%#x actual=%#x",
                     forbidden, sites);
        // Keep receipt and payload invariants separate. A missing safe split
        // first identifies the exact missing consumer bit; a branch which
        // emits the receipt but still enumerates fields fails this invariant.
        CHECK_DETAIL(std::all_of(managedIteratorVisits.begin(), managedIteratorVisits.end(),
                                 [](size_t count) { return count == 0; }),
                     "invisible segmented-array safe iterator exposed payload");
        g_managedSegmentedActive.store(false, std::memory_order_release);
    }
    if (g_largeArrayInitTestHooks.onWithdraw != nullptr) {
        g_largeArrayInitTestHooks.onWithdraw(complete);
    }
#endif
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
