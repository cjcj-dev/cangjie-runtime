// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Collector/ReferenceProcessor.h"

#include <new>

#include "Base/Panic.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::function<void()> g_beforeWeakCleanCasForTest;
}
#endif

ReferenceProcessor::ReferenceProcessor()
{
    for (size_t i = 0; i < REFERENCE_TYPE_COUNT; ++i) {
        encountered[i].store(0, std::memory_order_relaxed);
        discovered[i].store(0, std::memory_order_relaxed);
        enqueued[i].store(0, std::memory_order_relaxed);
    }
}

ReferenceProcessor::~ReferenceProcessor()
{
    DeleteList(discoveredList.exchange(nullptr, std::memory_order_acq_rel));
    DeleteList(pendingList.exchange(nullptr, std::memory_order_acq_rel));
}

void ReferenceProcessor::Push(std::atomic<Node*>& head, Node* node)
{
    Node* old = head.load(std::memory_order_relaxed);
    do {
        node->next = old;
    } while (!head.compare_exchange_weak(old, node, std::memory_order_release, std::memory_order_relaxed));
}

void ReferenceProcessor::DeleteList(Node* list)
{
    while (list != nullptr) {
        Node* next = list->next;
        delete list;
        list = next;
    }
}

ReferenceStatus ReferenceProcessor::DiscoverReference(BaseObject* reference, ReferenceType type)
{
    CHECK(TypeIndex(type) < TypeIndex(ReferenceType::COUNT));
    encountered[TypeIndex(type)].fetch_add(1, std::memory_order_relaxed);
    if (!IsSupported(type)) {
        return ReferenceStatus::UNSUPPORTED;
    }
    if (reference == nullptr) {
        return ReferenceStatus::INACTIVE;
    }
    Node* node = new (std::nothrow) Node{ reference, type, nullptr };
    CHECK(node != nullptr);
    Push(discoveredList, node);
    discovered[TypeIndex(type)].fetch_add(1, std::memory_order_relaxed);
    return ReferenceStatus::DISCOVERED;
}

void ReferenceProcessor::ProcessReferences(const IsStronglyLive& isStronglyLive)
{
    Node* list = discoveredList.exchange(nullptr, std::memory_order_acq_rel);
    while (list != nullptr) {
        Node* node = list;
        list = list->next;
        node->next = nullptr;

        BaseObject* target = node->reference;
        bool pending = false;
        if (node->type == ReferenceType::WEAK) {
            HeapSlot<>& referentField =
                HeapSlotAt<>(reinterpret_cast<uintptr_t>(node->reference) + TYPEINFO_PTR_SIZE);
            target = to_object(referentField.GetTargetObject(std::memory_order_acquire));
            pending = target != nullptr && !isStronglyLive(target);
        } else if (node->type == ReferenceType::FINAL) {
            pending = IsFinalizable(target);
        }

        if (!pending) {
            delete node;
            continue;
        }
        Push(pendingList, node);
    }
}

bool ReferenceProcessor::IsFinalizable(BaseObject* reference)
{
    if (reference == nullptr || !Heap::IsHeapAddress(reference)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(reference));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return false;
    }
    return region->IsResurrectedObject(reference);
}

ReferenceProcessor::WeakCleanResult ReferenceProcessor::CleanWeakReferenceWithResult(BaseObject* reference)
{
    HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(reference) + TYPEINFO_PTR_SIZE);
    const zpointer observed = referentField.GetFieldValue(std::memory_order_acquire);
    BaseObject* referent = to_object(referentField.GetTargetObject(std::memory_order_acquire));
    if (referent == nullptr) {
        return { false, false, nullptr };
    }
    if (!Heap::IsHeapAddress(referent)) {
        return { false, false, referent };
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(referent));
    if (region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion()) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        if (region->IsMarkedObject(view, referent)) {
            return { false, false, referent };
        }
    }

    // ZReferenceProcessor::try_make_inactive uses a cleaning barrier rather
    // than a plain null store.  Reuse the product CAS primitive with the exact
    // observed coloured value so a concurrent mutator update wins.
#if defined(MRT_TESTABLE_INTERNALS)
    if (g_beforeWeakCleanCasForTest) {
        g_beforeWeakCleanCasForTest();
    }
#endif
    if (HealSlot(referentField, observed, to_zpointer(0), HealSite::PostTraceReadReference,
                 HealNull::Allow, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return { true, false, nullptr };
    }

    // The enqueue consumer must not retain the stale pre-CAS value.  Match the
    // load-barrier consumer's retry contract by observing the winning terminal
    // value after an exact-observed CAS loses.
    BaseObject* terminal = to_object(referentField.GetTargetObject(std::memory_order_acquire));
    return { false, true, terminal };
}

bool ReferenceProcessor::CleanWeakReference(BaseObject* reference)
{
    return CleanWeakReferenceWithResult(reference).cleared;
}

void ReferenceProcessor::EnqueueReferencesImpl(const EnqueueFinal& enqueueFinal,
                                                const ObserveWeakFinal& observeWeakFinal)
{
    Node* list = pendingList.exchange(nullptr, std::memory_order_acq_rel);
    while (list != nullptr) {
        Node* node = list;
        list = list->next;
        bool accepted = false;
        if (node->type == ReferenceType::WEAK) {
            const WeakCleanResult result = CleanWeakReferenceWithResult(node->reference);
            accepted = result.cleared;
            if (observeWeakFinal) {
                observeWeakFinal(node->reference, result.terminalReferent);
            }
        } else if (node->type == ReferenceType::FINAL) {
            enqueueFinal(node->reference);
            accepted = true;
        }
        if (accepted) {
            enqueued[TypeIndex(node->type)].fetch_add(1, std::memory_order_relaxed);
        }
        delete node;
    }
}

void ReferenceProcessor::EnqueueReferences(const EnqueueFinal& enqueueFinal)
{
    EnqueueReferencesImpl(enqueueFinal, ObserveWeakFinal{});
}

#if defined(MRT_TESTABLE_INTERNALS)
void ReferenceProcessor::EnqueueReferences(const EnqueueFinal& enqueueFinal,
                                           const ObserveWeakFinal& observeWeakFinal)
{
    EnqueueReferencesImpl(enqueueFinal, observeWeakFinal);
}

void ReferenceProcessor::SetBeforeWeakCleanCasForTest(std::function<void()> hook)
{
    g_beforeWeakCleanCasForTest = std::move(hook);
}
#endif

size_t ReferenceProcessor::Encountered(ReferenceType type) const
{
    CHECK(TypeIndex(type) < TypeIndex(ReferenceType::COUNT));
    return encountered[TypeIndex(type)].load(std::memory_order_acquire);
}

size_t ReferenceProcessor::Discovered(ReferenceType type) const
{
    CHECK(TypeIndex(type) < TypeIndex(ReferenceType::COUNT));
    return discovered[TypeIndex(type)].load(std::memory_order_acquire);
}

size_t ReferenceProcessor::Enqueued(ReferenceType type) const
{
    CHECK(TypeIndex(type) < TypeIndex(ReferenceType::COUNT));
    return enqueued[TypeIndex(type)].load(std::memory_order_acquire);
}

bool ReferenceProcessor::Empty() const
{
    return discoveredList.load(std::memory_order_acquire) == nullptr &&
        pendingList.load(std::memory_order_acquire) == nullptr;
}

} // namespace MapleRuntime
