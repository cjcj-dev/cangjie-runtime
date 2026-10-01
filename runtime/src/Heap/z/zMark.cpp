#include "Heap/z/zAbort.inline.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeap.hpp"
#include "Mutator/VMOperation.h"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zRootsIterator.hpp"

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

namespace {
// ZGC zMark.cpp:703-708,827-828,883: both generations use this thread closure.
// Cangjie has no return statepoint: retain the saved head color for the full
// scan and expand stack objects/headerless records before visiting heap slots.
class MarkThreadClosure {
public:
    MarkThreadClosure()
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
        StackWatermarkSet::finish_processing(mutator, reinterpret_cast<void*>(root_function()));
        ZThreadLocalAllocBuffer::update_stats(mutator);
    }
};
} // namespace

// ZReferenceProcessor::should_discover/discover (zReferenceProcessor.cpp:174-201,
// 239-250). Native registration owns the original referent slot, rather than a
// Java FinalReference object. The load barrier heals remapping before discovery.
void ZMark::DiscoverFinalizableRoot(NativeSlot& slot)
{
    CHECK(Heap::GetHeap().old().IsPhaseMark());
    BaseObject* object = to_object(ZBarrier::load_barrier_on_oop_field(reinterpret_cast<volatile zpointer*>(&(slot))));
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

// ZGC zMark.cpp:711-795. The scheduler holds the carrier lock while
// these closures inspect and publish the saved guard.
class MarkNMethodClosure {
public:
    void DoNMethod(CJThreadRoot& root)
    {
        if (!root.is_armed()) { return; }
        ZUncoloredRootMarkOopClosure closure(root.saved_color());
        OopClosure& mark = closure;
        root.oops_do([&](RootSlot& slot) { mark.do_oop(&HeapSlotAt<>(static_cast<void*>(&slot))); });
        root.guard_with(ZPointerStoreGoodMask);
    }
};
class MarkYoungNMethodClosure {
public:
    void DoNMethod(CJThreadRoot& root)
    {
        if (!root.is_armed()) { return; }
        const uintptr_t oldMarked = root.saved_color() & ZPointerMarkedOldMask;
        const uintptr_t nextColor = ZPointerLoadGoodMask | ZPointerMarkedYoung | oldMarked | ZPointerRemembered;
        ZUncoloredRootMarkYoungOopClosure closure(root.saved_color());
        OopClosure& mark = closure;
        root.oops_do([&](RootSlot& slot) { mark.do_oop(&HeapSlotAt<>(static_cast<void*>(&slot))); });
        root.guard_with(nextColor);
    }
};

// ZMarkOldRootsTask, zMark.cpp:797-834. Root results are published to the
// generation mark domain by closures, then flushed by each participating worker.
class MarkOldRootsTask final : public ZTask {
public:
    explicit MarkOldRootsTask(unsigned workers)
        : ZTask("ZMarkOldRootsTask"), rootsColored(workers, ZGenerationIdOptional::old),
          finalizerRoots(Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers),
          rootsUncolored(ZGenerationIdOptional::old) {}
    void work() override
    {
        finalizerRoots.OopsDo([](NativeSlot& slot) { ZMark::DiscoverFinalizableRoot(slot); });
        rootsColored.Apply([&](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
        });
        rootsUncolored.Apply([&](Mutator& mutator) {
            threadClosure.DoThread(mutator);
        }, [&](CJThreadRoot& root) { carrierClosure.DoNMethod(root); });
        // Cross-VM ownership roots remain pending alignment under #1334.
        if (!foreignClaimed.exchange(true, std::memory_order_relaxed)) {
            Heap::GetHeap().cross_vm().VisitSurrectedExportRoots([](BaseObject* object) {
                if (Heap::IsHeapAddress(object)) {
                    ZBarrier::Mark<false, false, true, false>(from_object(object));
                }
            });
        }
        // zMark.cpp:830-834: flush and free worker stacks for both generations
        // here, since the set of workers executing during root scanning can be
        // different from the set of workers executing during mark.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
private:
    RootsIteratorStrongColored rootsColored;
    OopStorage::ParState<true> finalizerRoots;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    MarkNMethodClosure carrierClosure;
    std::atomic<bool> foreignClaimed{false};
};
} // namespace

void ZMark::EnumAllCommonRoots(ZWorkers& workers)
{
    CHECK_DETAIL(Heap::GetHeap().old().MarkPtr() != nullptr, "old mark domain must start before roots");
    MarkOldRootsTask task(workers.active_workers());
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
    explicit MarkYoungRootsTask(unsigned workers)
        : ZTask("ZMarkYoungRootsTask"), rootsColored(workers, ZGenerationIdOptional::young),
          rootsUncolored(ZGenerationIdOptional::young) {}

    void work() override
    {
        rootsColored.Apply([this](NativeSlot& slot) {
            coloredClosure.DoOop(slot);
        });
        rootsUncolored.Apply([&](Mutator& mutator) { threadClosure.DoThread(mutator); },
                             [&](CJThreadRoot& root) { carrierClosure.DoNMethod(root); });
        // Cross-VM ownership roots remain pending alignment under #1334.
        if (!foreignClaimed.exchange(true, std::memory_order_relaxed)) {
            Heap::GetHeap().cross_vm().VisitMinorValueRoots([](BaseObject* object) {
                if (Heap::IsHeapAddress(object)) {
                    ZBarrier::Mark<false, false, true, false>(from_object(object));
                }
            });
        }
        // zMark.cpp:887-891: flush and free worker stacks for both generations.
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
private:
    RootsIteratorAllColored rootsColored;
    MarkYoungOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    MarkYoungNMethodClosure carrierClosure;
    std::atomic<bool> foreignClaimed{false};
    RootsIteratorAllUncolored rootsUncolored;
};
} // namespace

void ZMark::VisitMinorRoots()
{
    MarkYoungRootsTask task(Heap::GetHeap().young().Workers()->active_workers());
    SuspendibleThreadSetJoiner joiner;
    (*(*ZGeneration::young()).Workers()).run(&task);

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

bool ZMark::FollowWork(bool partial)
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext context(nworkers, workerId, stripes, Stacks());
    for (;;) {
        if (!Drain(context, workerId)) {
            terminate.Leave();
            return false;
        }
        if (StealLocalRound(context, stripes) ||
            StealGlobalRound(context, smr, stripes, workerId)) {
            continue;
        }
        if (partial) {
            return true;
        }
        if (TryProactiveFlush(workerId)) {
            continue;
        }
        if (terminate.TryTerminate(stripes, context.NStripes())) {
            return true;
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
    (*ZGeneration::generation(static_cast<ZGenerationId>(statId))).StatMark()->AtMarkStart(targetNStripes);
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
    (void)FollowWork(false);
}

bool ZMark::FollowWorkPartial()
{
    return FollowWork(true);
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

// ZGC zMark.cpp:535-585: one closure for managed and native owners.
class ZMarkFlushStacksHandshakeClosure : public HandshakeClosure {
public:
    explicit ZMarkFlushStacksHandshakeClosure(ZMark* domain)
        : HandshakeClosure("ZMarkFlushStacks"), domain_(domain), flushed_(false) {}
    void do_thread(Mutator* thread) override { do_thread(thread->GetGCData()); }
    void do_thread(ThreadGCData& data)
    {
        if (FlushTargetGCData(data, domain_)) { flushed_ = true; }
    }
    bool flushed() const { return flushed_.load(std::memory_order_relaxed); }
private:
    ZMark* const domain_;
    std::atomic<bool> flushed_;
};

class VM_ZMarkFlushOperation final : public VMOperation {
public:
    explicit VM_ZMarkFlushOperation(ZMarkFlushStacksHandshakeClosure* cl) : cl_(cl) {}
    const char* name() const override { return "ZMarkFlushOperation"; }
    bool evaluate_at_safepoint() const override { return false; }
    bool is_gc_operation() const override { return true; }
    void doit() override
    {
        // A native VM thread has no Mutator; its owner is nativeGCData.
        cl_->do_thread(ThreadLocal::GetNativeGCData());
    }
private:
    ZMarkFlushStacksHandshakeClosure* const cl_;
};

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

    ZMarkFlushStacksHandshakeClosure cl(domain);
    VM_ZMarkFlushOperation vmCl(&cl);
    Handshake::execute(&cl);
    VMThread::execute(&vmCl);
    return cl.flushed();
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
    (*ZGeneration::generation(static_cast<ZGenerationId>(statId))).StatMark()->AtMarkEnd(nproactiveflush, nterminateflush,
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
