// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zMark.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <condition_variable>
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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/Handshake.h"
#include "Heap/z/zReferenceProcessor.hpp"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"

namespace MapleRuntime {

// RefFieldRoot is root in tagged pointer format.
void ZMark::EnumRefFieldRoot(RefField<>& field, ValueRootList& exportOwners)
{
    RefField<> oldField(field);
    CHECK_DETAIL(!Heap::IsHeapAddress(to_object(oldField.GetTargetObject())) ||
                     (raw(oldField.GetFieldValue()) &
                      (ZPointerRemappedMask | ZPointerMarkedYoungMask | ZPointerMarkedOldMask)) != 0,
                 "NativeSlot requires colored value at EnumRefFieldRoot slot=%p word=%#zx",
                 &field, raw(oldField.GetFieldValue()));
    ZBarrier::MarkBarrierOnOopField(field, false);
    BaseObject* latest = to_object(field.GetTargetObject());
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    // Ownership state carries current identity and color, never GC work entries.
    exportOwners.emplace_back(latest);
}




// Shared by mark roots and Cangjie foreign-root traversal.

namespace {
// VisitMinorRoots reports the same slots the watermark marks. ZGC publishes
// only into the mark stack (zMark.cpp:706); the product visitor is extra state
// the root-function pointer cannot carry.
thread_local const RootVisitor* markResultVisitor = nullptr;

void MarkAndReportRoot(zaddress_unsafe* p, uintptr_t color)
{
    ZUncoloredRoot::mark(p, color);
    if (markResultVisitor != nullptr) {
        (*markResultVisitor)(*reinterpret_cast<RootSlot*>(p));
    }
}

// ZGC zMark.cpp:703-708,827-828,883: both generations use this thread closure.
// Cangjie has no return statepoint: retain the saved head color for the full
// scan and expand stack objects/headerless records before visiting heap slots.
class MarkThreadClosure {
public:
    explicit MarkThreadClosure(const RootVisitor* result = nullptr) : result(result)
    {
        ZThreadLocalAllocBuffer::reset_statistics();
    }
    // ZGC zMark.cpp:699-701: publish TLAB statistics when root scanning ends.
    ~MarkThreadClosure()
    {
        ZThreadLocalAllocBuffer::publish_statistics();
    }
    static StackWatermarkProcessOopClosure::RootFunction root_function() { return ZUncoloredRoot::mark; }
    void DoThread(Mutator& mutator)
    {
        const RootVisitor* const previous = markResultVisitor;
        markResultVisitor = result;
        StackWatermarkProcessOopClosure::RootFunction const function =
            result == nullptr ? root_function() : MarkAndReportRoot;
        StackWatermarkSet::finish_processing(mutator, reinterpret_cast<void*>(function));
        markResultVisitor = previous;
        ZThreadLocalAllocBuffer::update_stats(mutator);
    }
private:
    const RootVisitor* const result;
};
} // namespace

// ZReferenceProcessor::should_discover/discover (zReferenceProcessor.cpp:174-201,
// 239-250). Native registration owns the original referent slot, rather than a
// Java FinalReference object. The load barrier heals remapping before discovery.
void ZMark::DiscoverFinalizableRoot(NativeSlot& slot)
{
    CHECK(Heap::GetHeap().old().IsPhaseMark());
    BaseObject* object = to_object(ZBarrier::load_barrier_on_oop_field(reinterpret_cast<volatile zpointer*>(&(slot))));
    object = ZBarrier::ValidateCurrentValue(object);
    if (object == nullptr) return;
    auto* page = Heap::page(reinterpret_cast<MAddress>(object));
    if (page->IsYoungRegion() || page->is_object_strongly_live(from_object(object))) return;
    auto& processor = Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor();
    (void)processor.discover_reference(object, ReferenceType::FINAL);
    ZBarrier::MarkFinalizableBarrierOnRoot(slot);
}


namespace {
// ZMarkOopClosure (zMark.cpp:666-670). P08 owns the missing dedicated old
// mark barrier; this adapter consumes the existing old publication producer.
    class MarkOopClosure {
    public:
        void DoOop(NativeSlot& slot) const
        {
            ZBarrier::MarkBarrierOnOopField(slot, false);
        }
    };

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public ZTask {
public:
    MarkOldRootsTask(ZMark& domain,
                     NativeSlotVisitor finalizable, std::function<void()> uncolored,
                     ValueRootList& exportOwners, unsigned workers)
        : ZTask("ZMarkOldRootsTask"), rootsColored(workers),
          finalizerRoots(Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers),
          exportRoots(Heap::GetHeap().GetExportRootStorage(), workers),
          finalizable(std::move(finalizable)), domain(domain), uncolored(std::move(uncolored)),
          exportOwners(exportOwners) {}
    void work() override
    {
        ValueRootList localExportOwners;
        exportRoots.OopsDo([&](NativeSlot& slot) {
            ZMark::EnumRefFieldRoot(slot, localExportOwners);
        });
        {
            std::lock_guard<std::mutex> lock(exportOwnersMutex);
            exportOwners.splice(exportOwners.end(), localExportOwners);
        }
        finalizerRoots.OopsDo(finalizable);
        rootsColored.Apply([&](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
        });
        rootsUncolored.Apply(uncolored);
        rootsUncolored.ApplyThreads([&](Mutator& mutator) {
            threadClosure.DoThread(mutator);
        });
        // zMark.cpp:830-834: flush and free worker stacks for both generations
        // here, since the set of workers executing during root scanning can be
        // different from the set of workers executing during mark.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
private:
    RootsIteratorStrongColored rootsColored;
    OopStorage::ParState<true> finalizerRoots;
    OopStorage::ParState<true> exportRoots;
    NativeSlotVisitor finalizable;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    ZMark& domain;
    std::function<void()> uncolored;
    ValueRootList& exportOwners;
    std::mutex exportOwnersMutex;
};
} // namespace

void ZMark::EnumAllCommonRoots(ZWorkers& workers, ValueRootList& exportOwners)
{
    CHECK_DETAIL(Heap::GetHeap().old().MarkPtr() != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(Heap::GetHeap().old().Mark(),
                         [](NativeSlot& slot) { DiscoverFinalizableRoot(slot); }, [&] {
        VisitStrongPlainRoots([&](ObjectRef& root) {
            ZUncoloredRoot::mark_object(safe(root.LoadPlain()));
        }, {});
        Heap::GetHeap().cross_vm().VisitSurrectedExportRoots([](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZBarrier::Mark<false, false, true, false>(from_object(object));
            }
        });
    }, exportOwners, workers.active_workers());
    workers.run(&task);
}

namespace {
// ZMarkYoungOopClosure, zMark.cpp:678-681.
class MarkYoungOopClosure {
public:
    void DoOop(NativeSlot& slot) const
    {
        ZBarrier::MarkYoungGoodBarrierOnOopField(slot);
    }
};

// ZMarkYoungRootsTask, zMark.cpp:852-891. Colored roots share one closure;
// Cangjie's stack/value-root scanner replaces HotSpot thread/nmethod closures.
class MarkYoungRootsTask final : public ZTask {
public:
    MarkYoungRootsTask(std::function<void()> uncolored, const RootVisitor& visitor, unsigned workers)
        : ZTask("ZMarkYoungRootsTask"), rootsColored(workers), threadClosure(&visitor),
          uncolored(std::move(uncolored)) {}

    void work() override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
        });
        rootsUncolored.Apply(uncolored);
        rootsUncolored.ApplyThreads([&](Mutator& mutator) { threadClosure.DoThread(mutator); });
        // zMark.cpp:887-891: flush and free worker stacks for both generations.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
private:
    RootsIteratorAllColored rootsColored;
    MarkYoungOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    std::function<void()> uncolored;
    RootsIteratorAllUncolored rootsUncolored;
};
} // namespace

void ZMark::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
                                 const std::function<void(BaseObject*)>& invisibleVisitor)
{
    // The C++ result container supplied by the young phase is shared by
    // root workers; marking itself still uses the worker-local mark stacks.
    std::mutex resultLock;
    const auto resultVisitor = [&visitor, &resultLock](BaseObject* object) {
        std::lock_guard<std::mutex> lock(resultLock);
        visitor(object);
    };
    RootVisitor rawRootVisitor = [&resultVisitor](ObjectRef& root) {
        resultVisitor(to_object(safe(root.LoadPlain())));
    };
    (void)invisibleVisitor; // Watermark owns the invisible slot with its saved color.
    MarkYoungRootsTask task([&] {
        VisitStrongPlainRoots(rawRootVisitor, {});
        Heap::GetHeap().cross_vm().VisitMinorValueRoots([&](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                ZBarrier::Mark<false, false, true, false>(from_object(object));
            }
            resultVisitor(object);
        });
        Heap::GetHeap().VisitAllExportRoots([&](NativeSlot& slot) {
            ZBarrier::MarkBarrierOnOopField(slot, false);
            resultVisitor(to_object(slot.GetTargetObject()));
        });
    }, rawRootVisitor, (*Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers()).active_workers());
    SuspendibleThreadSetJoiner joiner;
    (*Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers()).run(&task);

}

class ZMarkTask : public ZRestartableTask {
public:
    explicit ZMarkTask(ZMark* mark)
        : ZRestartableTask("ZMarkTask"), mark(mark)
    {
        mark->PrepareWork();
    }
    ~ZMarkTask() { mark->FinishWork(); }

    void resize_workers(uint32_t workers) override { mark->ResizeWorkers(workers); }

    void work() override
    {
        SuspendibleThreadSetJoiner stsJoiner;
        mark->FollowWorkComplete();
        Heap::GetHeap().mark_flush(ThreadLocal::GetGCData());
    }

private:
    ZMark* const mark;
};

void ZMark::ProcessFinalizers()
{
    FinalizerProcessor& fp = Heap::GetHeap().GetFinalizerProcessor();
    fp.ProcessReferences([](BaseObject* obj) { return RegionSpace::IsMarkedObject<Generation::Old>(obj); });
}

bool ZMark::PublishHandshakeMarkWork(WorkStack& work, ZMark* domain)
{
    if (domain == nullptr || work.empty()) {
        return false;
    }
    MarkThreadLocalStacks& seed = domain->Stacks();
    bool published = false;
    while (!work.empty()) {
        const MarkStackEntry entry = work.back();
        work.pop_back();
        MAddress address = 0;
        if (entry.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(entry, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(entry.object_address()))));
        }
        if (address == 0) {
            continue;
        }
        seed.Push(domain->Stripes(), domain->Stripes().StripeForAddress(address), entry, true);
        published = true;
    }
    if (published) {
        (void)seed.Flush(domain->Stripes());
        domain->Terminate().Wake();
    }
    return published;
}

bool ZMark::FlushGCDataMarkProducers(ThreadGCData& data, ZMark* domain)
{
    return domain != nullptr && data.FlushMarkStacks(*domain);
}

bool ZMark::FlushGCDataMarkProducers(ThreadGCData& data)
{
    const bool young = ZMark::FlushGCDataMarkProducers(data, Heap::GetHeap().young().MarkPtr());
    return ZMark::FlushGCDataMarkProducers(data, Heap::GetHeap().old().MarkPtr()) || young;
}

bool ZMark::FlushThreadMarkProducers(ThreadLocalData* tls)
{
    bool published = ZMark::FlushThreadMarkProducers(tls, Heap::GetHeap().young().MarkPtr());
    return ZMark::FlushThreadMarkProducers(tls, Heap::GetHeap().old().MarkPtr()) || published;
}

bool ZMark::FlushThreadMarkProducers(ThreadLocalData* tls, ZMark* domain)
{
    if (tls == nullptr || domain == nullptr) {
        return false;
    }
    return ThreadLocal::FlushMarkStacks(tls, *domain);
}
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"



// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zVerify.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"

#include <algorithm>
#include "Base/CString.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/RefField.inline.h"


namespace MapleRuntime {


} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMark.hpp"

#include "Base/Log.h"
#include "Heap/z/zWorkers.hpp"
#include "Mutator/MutatorManager.h"
#include "Heap/z/zMark.hpp"

namespace MapleRuntime {

static bool StealLocalRound(MarkContext& context, MarkStripeSet& stripes)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    MarkStripe* const home = context.Stripe();
    for (MarkStripe* victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = stacks.StealLocal(stripes, victim);
        if (stack != nullptr) {
            stacks.Install(stripes, home, stack);
            return true;
        }
    }
    return false;
}

static bool StealGlobalRound(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, size_t workerId)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    MarkStripe* const home = context.Stripe();
    for (MarkStripe* victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = victim->StealStack(smr, workerId);
        if (stack != nullptr) {
            stacks.Install(stripes, home, stack);
            return true;
        }
    }
    return false;
}

bool ZMark::RebalanceWork(MarkContext& context, size_t workerId)
{
    const size_t assumed = context.NStripes();
    const size_t nstripes = stripes.NStripes();
    if (assumed != nstripes) {
        context.SetNStripes(nstripes);
    } else if (nstripes < stripes.CalculateNStripes(nworkers) && stripes.IsCrowded()) {
        const size_t restored = nstripes << 1;
        if (stripes.TrySetNStripes(nstripes, restored)) {
            context.SetNStripes(restored);
        }
    }
    MarkStripe* const stripe = stripes.StripeForWorker(nworkers, workerId);
    if (context.Stripe() != stripe) {
        context.SetStripe(stripe);
        (void)context.Stacks().Flush(stripes);
    } else if (!terminate.Saturated()) {
        (void)context.Stacks().Flush(stripes);
    }
    return PollStop();
}

bool ZMark::Drain(MarkContext& context, size_t workerId)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripe(stripes.StripeForWorker(nworkers, workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.Stripe(), entry)) {
        MarkAndFollow(context, entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, workerId)) {
            return false;
        }
    }
    return true;
}

ZMark::Result ZMark::FollowWork(MarkContext& context, size_t workerId, bool partial)
{
    for (;;) {
        if (!Drain(context, workerId)) {
            terminate.Leave();
            return Result::Aborted;
        }
        if (StealLocalRound(context, stripes) ||
            StealGlobalRound(context, smr, stripes, workerId)) {
            continue;
        }
        if (partial) {
            return Result::Partial;
        }
        if (TryProactiveFlush(workerId)) {
            continue;
        }
        if (terminate.TryTerminate(stripes, context.NStripes())) {
            context.Cache().Flush();
            return Result::Completed;
        }
    }
}

ZMark::ZMark(size_t capacity, MarkingStacks::MarkingGeneration generation)
    : stripes(capacity), generation(generation)
{
    stripes.SetTerminate(&terminate);
}

size_t ZMark::CalculateNStripes(size_t workers) const
{
    return stripes.CalculateNStripes(workers);
}

void ZMark::Start()
{
    if (ZVerifyMarking) { verify_all_stacks_empty(); }

    nproactiveflush = 0;
    nterminateflush = 0;
    ntrycomplete = 0;
    ncontinue = 0;
    CHECK_DETAIL(gcWorkers != nullptr, "ZMark::start requires workers");
    nworkers = gcWorkers->active_workers();
    targetNStripes = CalculateNStripes(nworkers);
    stripes.SetNStripes(targetNStripes);
    terminate.Reset(nworkers);
    // zMark.cpp:118-123: stripe count goes to the generation's mark account.
    const ZGenerationId statId =
        generation == MarkingStacks::MarkingGeneration::YOUNG ? ZGenerationId::young : ZGenerationId::old;
    Heap::GetHeap().GetZGeneration(statId).StatMark()->AtMarkStart(targetNStripes);
}

void ZMark::PrepareWork()
{
    nworkers = gcWorkers->active_workers();
    targetNStripes = CalculateNStripes(nworkers);
    stripes.SetNStripes(targetNStripes);
    terminate.Reset(nworkers);
    workNProactiveFlush.store(0, std::memory_order_relaxed);
    workNTerminateFlush.store(0, std::memory_order_relaxed);
}

void ZMark::FollowWorkComplete()
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    (void)FollowWork(local, workerId, false);
    (void)local.Stacks().Flush(stripes);
    local.Cache().Flush();
}

bool ZMark::FollowWorkPartial()
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    const Result result = FollowWork(local, workerId, true);
    (void)local.Stacks().Flush(stripes);
    local.Cache().Flush();
    return result != Result::Aborted;
}

void ZMark::MarkFollow()
{
    for (;;) {
        ZMarkTask task(this);
        gcWorkers->run(&task);
        if (ZAbort::should_abort() || !TryTerminateFlush()) {
            break;
        }
    }
}

// ZGC zMark.cpp:173-175: classify reference arrays before choosing the follower.
bool ZMark::is_array(zaddress address) const
{
    TypeInfo* const type = to_object(address)->GetTypeInfo();
    TypeInfo* const component = type->IsRawArray() ? type->GetComponentTypeInfo() : nullptr;
    return component != nullptr &&
           (component->IsObjectType() || component->IsArrayType() || component->IsInterface());
}

void ZMark::MarkAndFollow(MarkContext& ctx, const MarkStackEntry& entry)
{
    const bool finalizable = entry.finalizable();
    if (entry.partial_array()) {
        follow_partial_array(ctx, entry, finalizable);
        return;
    }

    const zaddress address = ZOffset::address(to_zoffset(entry.object_address()));
    const bool mark = entry.mark();
    bool incLive = entry.inc_live();
    const bool follow = entry.follow();
    ZPage* const page = Heap::page(raw(address));
    CHECK_DETAIL(page->IsRelocatable(), "mark consumer requires a relocatable page");
    if (mark && !page->mark_object(address, finalizable, incLive)) {
        return;
    }
    BaseObject* const object = to_object(address);
    if (incLive) {
        const size_t bytes = AlignUp(object->GetSize(), page->object_alignment());
        ctx.Cache().IncLive(page, bytes);
    }
    if (follow) {
        if (is_array(address)) {
            follow_array_object(ctx, reinterpret_cast<MArray*>(object), finalizable);
        } else {
            follow_object(object, finalizable);
        }
    }
}

void ZMark::ResizeWorkers(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    terminate.Reset(workers);
}

void ZMark::FinishWork()
{
    nproactiveflush += workNProactiveFlush.load(std::memory_order_relaxed);
    nterminateflush += workNTerminateFlush.load(std::memory_order_relaxed);
}

bool ZMark::PollStop()
{
    if (ZAbort::should_abort()) {
        return true;
    }
    if (gcWorkers != nullptr && gcWorkers->should_worker_resize()) {
        return true;
    }
    return false;
}

MarkThreadLocalStacks& ZMark::Stacks()
{
    return ThreadLocal::GetMarkStacks(*this);
}

bool ZMark::FlushStacks()
{
    return ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), *this);
}

namespace {
bool FlushTargetGCData(ThreadGCData& data, ZMark* domain)
{
    if (domain != nullptr) { return domain->Flush(data); }
    const bool young = Heap::GetHeap().young().Mark().Flush(data);
    return Heap::GetHeap().old().Mark().Flush(data) || young;
}

} // namespace

bool ZMark::FlushThreadLocal(ThreadLocalData* tls, ZMark* domain)
{
    if (tls == nullptr) {
        return false;
    }
    bool published = false;
    if (tls->nativeGCData != nullptr && tls->nativeGCData != tls->gcData) {
        published = FlushTargetGCData(*tls->nativeGCData, domain);
    }
    if (tls->gcData != nullptr) {
        published = FlushTargetGCData(*tls->gcData, domain) || published;
    }
    return (domain == nullptr ? ZMark::FlushThreadMarkProducers(tls)
                              : ZMark::FlushThreadMarkProducers(tls, domain)) || published;
}

bool ZMark::HandshakeFlush(ZMark* domain)
{
    auto& manager = MutatorManager::Instance();
    bool flushed = false;
    if (manager.WorldStopped()) {
        // ZGC zMark.cpp:954-970: pause rendezvous excludes native producers;
        // the NJT Iterator separately protects the complete owner inventory.
        for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) {
            flushed = FlushTargetGCData(it.Current()->nativeData, domain) || flushed;
        }
        return flushed;
    }

    class ZMarkFlushStacksHandshakeClosure : public HandshakeClosure {
    public:
        explicit ZMarkFlushStacksHandshakeClosure(ZMark* d)
            : HandshakeClosure("ZMarkFlushStacks"), domain_(d), flushed_(false) {}
        void do_thread(Mutator* thread) override
        {
            // ZGC zMark.cpp:535-557: no per-owner lock. The closure runs on
            // the owner itself, or on the handshaker while the owner is
            // observed safe; a saferegion-resident owner never touches its
            // marking state (the exit flush runs before EnterSaferegion).
            if (FlushTargetGCData(thread->GetGCData(), domain_)) {
                flushed_ = true;
            }
        }
        bool flushed() const { return flushed_.load(std::memory_order_relaxed); }
    private:
        ZMark* domain_;
        std::atomic<bool> flushed_;
    } cl(domain);
    Handshake::execute(&cl);
    flushed = FlushThreadLocal(ThreadLocal::GetThreadLocalData(), domain) || flushed;
    if (cl.flushed()) {
        flushed = true;
    }
    return flushed;
}

bool ZMark::Flush()
{
    return HandshakeFlush(this) || !stripes.IsEmpty();
}

// ZGC zMark.cpp:998-1004: buffer processing may produce more stack work.
bool ZMark::Flush(ThreadGCData& data)
{
    if (data.managedOwner) {
        data.storeBarrierBuffer->Flush();
    }
    return data.FlushMarkStacks(*this);
}

bool ZMark::Flush(ThreadLocalData* tls)
{
    return FlushThreadLocal(tls, this);
}

bool ZMark::FlushThread(ThreadLocalData* tls)
{
    return FlushThreadLocal(tls, nullptr);
}

bool ZMark::FlushAllGenerations()
{
    return HandshakeFlush(nullptr);
}

bool ZMark::TryProactiveFlush(size_t workerId)
{
    constexpr size_t proactiveFlushMax = 10;
    if (workerId != 0) {
        return false;
    }
    if (workNProactiveFlush.load(std::memory_order_relaxed) == proactiveFlushMax) {
        return false;
    }
    workNProactiveFlush.fetch_add(1, std::memory_order_relaxed);
    SuspendibleThreadSetLeaver stsLeaver;
    return Flush();
}

bool ZMark::TryTerminateFlush()
{
    workNTerminateFlush.fetch_add(1, std::memory_order_relaxed);
    terminate.SetResurrected(false);
    if (ZVerifyMarking) { verify_worker_stacks_empty(); }
    return Flush() || terminate.Resurrected();
}

bool ZMark::TryEnd()
{
    if (terminate.Resurrected()) {
        return false;
    }
    (void)Flush(ThreadLocal::GetThreadLocalData());
    // zMark.cpp:954-970: resurrected, then non-Java flush; empty stripes => complete.
    (void)HandshakeFlush(this);
    (void)FlushStacks();
    if (!stripes.IsEmpty()) {
        return false;
    }
    return true;
}

bool ZMark::End()
{
    if (!TryEnd()) {
        ++ncontinue;
        return false;
    }
    if (ZVerifyMarking) { verify_all_stacks_empty(); }
    // zMark.cpp:983-987: completed mark publishes its flush/continue counters.
    const ZGenerationId statId =
        generation == MarkingStacks::MarkingGeneration::YOUNG ? ZGenerationId::young : ZGenerationId::old;
    Heap::GetHeap().GetZGeneration(statId).StatMark()->AtMarkEnd(nproactiveflush, nterminateflush,
                                                                 ntrycomplete, ncontinue);
    return true;
}

void ZMark::Free()
{
    smr.free();
}

} // namespace MapleRuntime

#include "Heap/z/zMarkTerminate.inline.hpp"

// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zMark.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
namespace MapleRuntime {
// ZGC zMark.cpp:1022-1035. The coordinator inspects the containers owned
// by each thread; verification neither flushes nor creates work.
void ZMark::verify_all_stacks_empty() const
{
    const size_t index = generation == MarkingStacks::MarkingGeneration::YOUNG ? 0 : 1;
    MutatorManager::Instance().VisitMarkingThreads([&](const ThreadGCData* data) {
        CHECK_DETAIL(data->markStacks[index].IsEmpty(),
                     "Thread marking stack is not empty: owner=%p generation=%zu", data, index);
    });
    CHECK_DETAIL(stripes.IsEmpty(), "Shared marking stripes are not empty");
}

void ZMark::verify_worker_stacks_empty() const
{
    const size_t index = generation == MarkingStacks::MarkingGeneration::YOUNG ? 0 : 1;
    gcWorkers->threads_do([&](WorkerThread* worker) {
        const ThreadGCData* data = worker->gc_data();
        CHECK_DETAIL(data != nullptr && data->markStacks[index].IsEmpty(),
                     "Worker marking stack is not empty: worker=%p generation=%zu", worker, index);
    });
}
}

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkPartialArray.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace MarkPartialArray {
// zMark.cpp:177-183 encode_partial_array_offset / decode_partial_array_offset.
MarkStackEntry Encode(const void* chunkStart, size_t length, bool finalizable)
{
    const MAddress addr = reinterpret_cast<MAddress>(chunkStart);
    DCHECK_D((addr & (MIN_SIZE - 1)) == 0, "Address misaligned");
    const size_t offset = untype(ZAddress::offset(to_zaddress(addr))) >> MIN_SIZE_SHIFT;
    return MarkStackEntry(offset, length, finalizable);
}

void Decode(const MarkStackEntry& entry, MAddress& chunkStart, size_t& length)
{
    const size_t offset = entry.partial_array_offset();
    length = entry.partial_array_length();
    chunkStart = raw(ZOffset::address(to_zoffset(offset << MIN_SIZE_SHIFT)));
}

} // namespace MarkPartialArray

// ZGC zMark.cpp:185-196: worker continuations remain local until a full
// stack is published by MarkThreadLocalStacks::Push.
void ZMark::push_partial_array(MarkContext& ctx, MAddress start, size_t length, bool finalizable)
{
    MarkStripe* const stripe = stripes.StripeForAddress(start);
    const MarkStackEntry entry = MarkPartialArray::Encode(reinterpret_cast<const void*>(start), length, finalizable);
    ctx.Stacks().Push(stripes, stripe, entry, false);
}

static void mark_barrier_on_oop_array(MAddress start, size_t length, bool finalizable, bool young)
{
    for (size_t i = 0; i < length; ++i) {
        auto& field = HeapSlotAt<>(start + i * sizeof(MAddress));
        if (young) {
            ZBarrier::MarkBarrierOnYoungOopField(field);
        } else {
            ZBarrier::MarkBarrierOnOldOopField(field, finalizable);
        }
    }
}

void ZMark::follow_array_elements_small(MAddress start, size_t length, bool finalizable)
{
    DCHECK_D(length <= MarkPartialArray::MIN_LENGTH, "Too large, should be split");
    mark_barrier_on_oop_array(start, length, finalizable,
                              generation == MarkingStacks::MarkingGeneration::YOUNG);
}

void ZMark::follow_array_elements_large(MarkContext& ctx, MAddress start, size_t length, bool finalizable)
{
    using namespace MarkPartialArray;
    DCHECK_D(length > MIN_LENGTH, "Too small, should not be split");
    const MAddress end = start + length * sizeof(MAddress);
    const MAddress middleStart = AlignUp(start + sizeof(MAddress), MIN_SIZE);
    const size_t middleLength = AlignDown((end - middleStart) / sizeof(MAddress), MIN_LENGTH);
    const MAddress middleEnd = middleStart + middleLength * sizeof(MAddress);
    if (end > middleEnd) {
        push_partial_array(ctx, middleEnd, (end - middleEnd) / sizeof(MAddress), finalizable);
    }
    MAddress part = middleEnd;
    while (part > middleStart) {
        const size_t count = AlignUp((part - middleStart) / sizeof(MAddress) / 2, MIN_LENGTH);
        part -= count * sizeof(MAddress);
        push_partial_array(ctx, part, count, finalizable);
    }
    follow_array_elements_small(start, (middleStart - start) / sizeof(MAddress), finalizable);
}

void ZMark::follow_array_elements(MarkContext& ctx, MAddress start, size_t length, bool finalizable)
{
    if (length <= MarkPartialArray::MIN_LENGTH) {
        follow_array_elements_small(start, length, finalizable);
    } else {
        follow_array_elements_large(ctx, start, length, finalizable);
    }
}

void ZMark::follow_partial_array(MarkContext& ctx, const MarkStackEntry& entry, bool finalizable)
{
    MAddress start = 0;
    size_t length = 0;
    MarkPartialArray::Decode(entry, start, length);
    follow_array_elements(ctx, start, length, finalizable);
}

// ZGC zMark.cpp:273-313: discovery is closure state, not an object-kind
// branch in mark_and_follow. Finalizable traversal follows the referent.
template <bool finalizable, ZGenerationIdOptional generation>
class ZMarkBarrierFollowOopClosure : public OopIterateClosure {
    static ReferenceDiscoverer* discoverer()
    {
        if (!finalizable) {
            return ZGeneration::old()->reference_discoverer();
        } else {
            return nullptr;
        }
    }
public:
    ZMarkBarrierFollowOopClosure() : OopIterateClosure(discoverer()) {}
    void do_oop(RefField<>* field) override
    {
        switch (generation) {
            case ZGenerationIdOptional::young:
                ZBarrier::MarkBarrierOnYoungOopField(*field);
                break;
            case ZGenerationIdOptional::old:
                ZBarrier::MarkBarrierOnOldOopField(*field, finalizable);
                break;
            case ZGenerationIdOptional::none:
                ZBarrier::MarkBarrierOnOopField(*field, finalizable);
                break;
        }
    }
};

// ZGC zMark.cpp:371-392: select the static closure before VM enumeration.
void ZMark::follow_object(BaseObject* object, bool finalizable)
{
    if (generation == MarkingStacks::MarkingGeneration::MAJOR) {
        if (finalizable) {
            ZMarkBarrierFollowOopClosure<true, ZGenerationIdOptional::old> closure;
            ZIterator::oop_iterate(object, &closure);
        } else {
            ZMarkBarrierFollowOopClosure<false, ZGenerationIdOptional::old> closure;
            ZIterator::oop_iterate(object, &closure);
        }
    } else {
        ZMarkBarrierFollowOopClosure<false, ZGenerationIdOptional::young> closure;
        ZIterator::oop_iterate(object, &closure);
    }
}

void ZMark::follow_array_object(MarkContext& ctx, MArray* array, bool finalizable)
{
    // Cangjie TypeInfo is native metadata; reference-array elements use the
    // same worker continuation path as ZGC zMark.cpp:346-368.
    follow_array_elements(ctx, reinterpret_cast<MAddress>(array->ConvertToCArray()),
                          array->GetLength(), finalizable);
}

} // namespace MapleRuntime

#include "Heap/z/zMark.inline.hpp"

