// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MArray.inline.h"

#include <algorithm>
#if defined(MRT_GC_UNIT_TESTS)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/GcRequest.h"
#include "Heap/Heap.h"
#endif

#include "Base/MemUtils.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/Collector/GcStats.h"
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
#endif

MArray* MArray::InitializeLargeRefArray(MAddress address, MSize arraySize, MIndex nElems,
                                        TypeInfo& arrayClass)
{
#if defined(MRT_GC_UNIT_TESTS)
    const ManagedSegmentedGc managedTestGc = GetManagedSegmentedGc();
    const bool managedTest = managedTestGc != ManagedSegmentedGc::NONE;
    bool managedTestRequested = false;
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
            if (managedTest && !managedTestRequested && segmentIndex == 0) {
                managedTestRequested = true;
                CHECK_DETAIL(mutator->IsManagedContext(),
                             "language-level segmented-array test must retain managed context");
                const size_t gcCountBefore = g_gcCount.load(std::memory_order_acquire);
                if (managedTestGc == ManagedSegmentedGc::YOUNG) {
                    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
                    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
                    size_t attempts = 0;
                    while (!resources.IsGcStarted() && attempts++ < 1000000) {
                        std::this_thread::yield();
                    }
                    CHECK_DETAIL(resources.IsGcStarted(),
                                 "language-level segmented-array young GC did not start");
                    resources.WaitForGCFinish();
                } else {
                    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_FORCE, false);
                }
                CHECK_DETAIL(g_gcCount.load(std::memory_order_acquire) > gcCountBefore,
                             "language-level segmented-array GC did not advance the epoch");
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
    if (managedTest) {
        const uint32_t required = managedTestGc == ManagedSegmentedGc::YOUNG
            ? VisitBit(LargeArrayRootVisitSite::MUTATOR_STACK_MANAGED) |
                VisitBit(LargeArrayRootVisitSite::MINOR_MARK) |
                VisitBit(LargeArrayRootVisitSite::MINOR_RELOCATE) |
                VisitBit(LargeArrayRootVisitSite::ITERATOR_SKIP)
            : VisitBit(LargeArrayRootVisitSite::STACK_WATERMARK_MANAGED) |
                VisitBit(LargeArrayRootVisitSite::MINOR_RELOCATE) |
                VisitBit(LargeArrayRootVisitSite::REMEMBERED) |
                VisitBit(LargeArrayRootVisitSite::ITERATOR_SKIP);
        const uint32_t sites = g_managedSegmentedVisitSites.load(std::memory_order_acquire);
        CHECK_DETAIL((sites & required) == required,
                     "language-level segmented-array GC missed managed root consumer: required=%#x actual=%#x",
                     required, sites);
        std::fprintf(stderr, "[SEGMENTED_MANAGED_OK] mode=%s root_sites=%#x\n",
                     managedTestGc == ManagedSegmentedGc::YOUNG ? "young" : "full", sites);
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
