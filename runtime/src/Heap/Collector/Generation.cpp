// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/WCollector/WCollector.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "Concurrency/Concurrency.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
struct Y2yHandoffReceiptState {
    std::atomic<uint64_t> phase0 { 0 };
    std::atomic<uint64_t> phase1 { 0 };
    std::atomic<uint64_t> phase2 { 0 };
    std::atomic<uint64_t> beforeRelease { 0 };
    std::atomic<uint64_t> afterRoot { 0 };
    std::atomic<uint64_t> afterStw2 { 0 };
};
Y2yHandoffReceiptState g_y2yHandoffReceipt;
std::atomic<BaseObject*> g_y2yAfterReleaseHolder { nullptr };
std::atomic<uint64_t> g_y2yAfterReleasePublications { 0 };
std::atomic<Mutator*> g_markBeforeMarkEndProducer { nullptr };
std::atomic<BaseObject*> g_markBeforeMarkEndFirst { nullptr };
std::atomic<BaseObject*> g_markBeforeMarkEndSecond { nullptr };
std::atomic<uint64_t> g_markBeforeMarkEndPublications { 0 };
std::atomic<BaseObject*> g_y2yDuringConcurrent { nullptr };
std::atomic<BaseObject*> g_leftoverY2yBeforePause { nullptr };
std::atomic<Mutator*> g_exportRootAfterT1Producer { nullptr };
std::atomic<BaseObject*> g_exportRootAfterT1Holder { nullptr };
std::atomic<BaseObject*> g_exportRootAfterT1Child { nullptr };
std::atomic<uint64_t> g_exportRootAfterT1Armed { 0 };
std::atomic<uint64_t> g_exportRootRegistrationsAfterT1 { 0 };
std::atomic<uint64_t> g_exportRootProducerFlushes { 0 };
std::atomic<uint64_t> g_exportRootObservedAtT2 { 0 };
std::atomic<U64> g_exportRootHandle { std::numeric_limits<U64>::max() };
std::atomic<bool> g_exportRootHolderMarked { false };
std::atomic<bool> g_exportRootChildMarked { false };
}

void ResetY2yHandoffTestReceipt()
{
    g_y2yHandoffReceipt.phase0.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.phase1.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.phase2.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.beforeRelease.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterRoot.store(0, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterStw2.store(0, std::memory_order_relaxed);
    g_y2yAfterReleaseHolder.store(nullptr, std::memory_order_relaxed);
    g_y2yAfterReleasePublications.store(0, std::memory_order_relaxed);
}

Y2yHandoffTestReceipt ReadY2yHandoffTestReceipt()
{
    return { g_y2yHandoffReceipt.phase0.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.phase1.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.phase2.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.beforeRelease.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.afterRoot.load(std::memory_order_relaxed),
             g_y2yHandoffReceipt.afterStw2.load(std::memory_order_relaxed) };
}

void NoteY2yBeforeReleaseTestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase0.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.beforeRelease.store(pending, std::memory_order_relaxed);
}

void NoteY2yAfterRootTestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase1.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterRoot.store(pending, std::memory_order_relaxed);
}

void NoteY2yAfterStw2TestReceipt(uint64_t pending)
{
    g_y2yHandoffReceipt.phase2.fetch_add(1, std::memory_order_relaxed);
    g_y2yHandoffReceipt.afterStw2.fetch_add(pending, std::memory_order_relaxed);
}

void ArmY2yAfterReleaseTestReceipt(BaseObject* holder, uint64_t publications)
{
    g_y2yAfterReleaseHolder.store(holder, std::memory_order_release);
    g_y2yAfterReleasePublications.store(publications, std::memory_order_release);
}

void PublishY2yAfterReleaseTestReceipt()
{
    auto claimPublication = [](std::atomic<uint64_t>& publications) {
        uint64_t remaining = publications.load(std::memory_order_acquire);
        while (remaining != 0 &&
               !publications.compare_exchange_weak(remaining, remaining - 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {}
        return remaining != 0;
    };
    if (claimPublication(g_y2yAfterReleasePublications)) {
        BaseObject* holder = g_y2yAfterReleaseHolder.load(std::memory_order_acquire);
        CHECK_DETAIL(holder != nullptr, "armed y2y after-release receipt without holder");
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(holder);
    }
}

void ArmMarkBeforeMarkEndTestReceipt(Mutator* producer, BaseObject* first, BaseObject* second)
{
    g_markBeforeMarkEndProducer.store(producer, std::memory_order_release);
    g_markBeforeMarkEndFirst.store(first, std::memory_order_release);
    g_markBeforeMarkEndSecond.store(second, std::memory_order_release);
    g_markBeforeMarkEndPublications.store(second == nullptr ? 1 : 2, std::memory_order_release);
}

void PublishMarkBeforeMarkEndTestReceipt()
{
    uint64_t remaining = g_markBeforeMarkEndPublications.load(std::memory_order_acquire);
    while (remaining != 0 &&
           !g_markBeforeMarkEndPublications.compare_exchange_weak(remaining, remaining - 1,
                                                                  std::memory_order_acq_rel,
                                                                  std::memory_order_acquire)) {}
    if (remaining == 0) {
        return;
    }
    Mutator* producer = g_markBeforeMarkEndProducer.load(std::memory_order_acquire);
    BaseObject* first = g_markBeforeMarkEndFirst.load(std::memory_order_acquire);
    BaseObject* second = g_markBeforeMarkEndSecond.load(std::memory_order_acquire);
    BaseObject* object = remaining == 2 ? first : (second != nullptr ? second : first);
    CHECK_DETAIL(producer != nullptr && object != nullptr, "armed mark-end receipt without producer/object");
    Heap::GetHeap().GetCollector().MarkObjectIfActive(object);
    producer->FlushStoreBarrierBuffer();
}

void ArmY2yDuringConcurrentTestReceipt(BaseObject* holder)
{
    g_y2yDuringConcurrent.store(holder, std::memory_order_release);
}

void PublishConcurrentYoungProducersTestReceipt()
{
    BaseObject* y2y = g_y2yDuringConcurrent.exchange(nullptr, std::memory_order_acq_rel);
    if (y2y != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(y2y);
    }
}

void ArmLeftoverBeforePauseTestReceipt(BaseObject* y2yHolder)
{
    g_leftoverY2yBeforePause.store(y2yHolder, std::memory_order_release);
}

void PublishLeftoverBeforePauseTestReceipt()
{
    BaseObject* y2y = g_leftoverY2yBeforePause.exchange(nullptr, std::memory_order_acq_rel);
    if (y2y != nullptr) {
        AllocBuffer::GetOrCreateAllocBuffer()->PushY2yDirtyHolder(y2y);
    }
}

void ResetExportRootPublicationTestReceipt()
{
    g_exportRootAfterT1Producer.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Holder.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Child.store(nullptr, std::memory_order_relaxed);
    g_exportRootAfterT1Armed.store(0, std::memory_order_relaxed);
    g_exportRootRegistrationsAfterT1.store(0, std::memory_order_relaxed);
    g_exportRootProducerFlushes.store(0, std::memory_order_relaxed);
    g_exportRootObservedAtT2.store(0, std::memory_order_relaxed);
    g_exportRootHandle.store(std::numeric_limits<U64>::max(), std::memory_order_relaxed);
    g_exportRootHolderMarked.store(false, std::memory_order_relaxed);
    g_exportRootChildMarked.store(false, std::memory_order_relaxed);
}

void ArmExportRootAfterT1TestReceipt(Mutator* producer, BaseObject* holder, BaseObject* child)
{
    CHECK_DETAIL(producer != nullptr && holder != nullptr && child != nullptr,
                 "export-root T1 receipt requires producer, holder, and child");
    g_exportRootAfterT1Producer.store(producer, std::memory_order_release);
    g_exportRootAfterT1Holder.store(holder, std::memory_order_release);
    g_exportRootAfterT1Child.store(child, std::memory_order_release);
    g_exportRootAfterT1Armed.store(1, std::memory_order_release);
}

void PublishExportRootAfterT1TestReceipt()
{
    if (g_exportRootAfterT1Armed.exchange(0, std::memory_order_acq_rel) == 0) {
        return;
    }
    Mutator* producer = g_exportRootAfterT1Producer.load(std::memory_order_acquire);
    BaseObject* holder = g_exportRootAfterT1Holder.load(std::memory_order_acquire);
    CHECK_DETAIL(producer != nullptr && holder != nullptr, "armed export-root T1 receipt is incomplete");
    Mutator* previous = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(producer);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(holder);
    ThreadLocal::SetMutator(previous);
    g_exportRootHandle.store(handle, std::memory_order_release);
    g_exportRootRegistrationsAfterT1.fetch_add(1, std::memory_order_relaxed);
}

void FlushExportRootAfterT1TestReceipt()
{
    if (g_exportRootRegistrationsAfterT1.load(std::memory_order_acquire) == 0 ||
        g_exportRootProducerFlushes.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    Mutator* producer = g_exportRootAfterT1Producer.load(std::memory_order_acquire);
    CHECK_DETAIL(producer != nullptr, "registered export-root T1 receipt has no producer");
    producer->FlushStoreBarrierBuffer();
}

void NoteExportRootPublicationAtT2TestReceipt()
{
    if (g_exportRootRegistrationsAfterT1.load(std::memory_order_acquire) == 0) {
        return;
    }
    BaseObject* holder = g_exportRootAfterT1Holder.load(std::memory_order_acquire);
    BaseObject* child = g_exportRootAfterT1Child.load(std::memory_order_acquire);
    auto isMarked = [](BaseObject* object) {
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        return region != nullptr && region->is_object_strongly_live(from_object(object));
    };
    g_exportRootHolderMarked.store(isMarked(holder), std::memory_order_relaxed);
    g_exportRootChildMarked.store(isMarked(child), std::memory_order_relaxed);
    g_exportRootObservedAtT2.fetch_add(1, std::memory_order_relaxed);
}

ExportRootPublicationTestReceipt ReadExportRootPublicationTestReceipt()
{
    return { g_exportRootRegistrationsAfterT1.load(std::memory_order_relaxed),
             g_exportRootProducerFlushes.load(std::memory_order_relaxed),
             g_exportRootObservedAtT2.load(std::memory_order_relaxed),
             g_exportRootHandle.load(std::memory_order_relaxed),
             g_exportRootHolderMarked.load(std::memory_order_relaxed),
             g_exportRootChildMarked.load(std::memory_order_relaxed) };
}

#endif
}
