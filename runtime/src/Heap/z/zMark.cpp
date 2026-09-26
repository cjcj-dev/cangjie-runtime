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
#include "Heap/z/zUncoloredRoot.hpp"
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
                     NativeSlotVisitor finalizable, std::function<void()> uncolored, unsigned workers)
        : ZTask("ZMarkOldRootsTask"), rootsColored(workers),
          finalizerRoots(Heap::GetHeap().GetFinalizerProcessor().WeakRootStorage(), workers),
          finalizable(std::move(finalizable)), domain(domain), uncolored(std::move(uncolored)) {}
    void work() override
    {
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
    NativeSlotVisitor finalizable;
    RootsIteratorStrongUncolored rootsUncolored;
    MarkOopClosure coloredClosure;
    MarkThreadClosure threadClosure;
    ZMark& domain;
    std::function<void()> uncolored;
};
} // namespace

void ZMark::EnumAllCommonRoots(ZWorkers& workers)
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
    }, workers.active_workers());
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

void ZMark::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin)
{
    PushYoungObject(object, workStack, origin, false);
}

void ZMark::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin,
                                  bool finalizable)
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    CHECK_DETAIL(object->IsValidObject(), "minor root/reference %p is not a valid object origin=%s",
                 object, origin);
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
    if (!region->IsYoungRegion()) {
        return;
    }
    (void)workStack;
    if (finalizable) {
        const_cast<ZGeneration&>(Heap::GetHeap().GetZGeneration(ZGenerationId::young))
            .MarkObjectIfActive<false, true, true, true>(from_object(object));
    } else {
        const_cast<ZGeneration&>(Heap::GetHeap().GetZGeneration(ZGenerationId::young))
            .MarkObjectIfActive<false, true, true, false>(from_object(object));
    }
}

// Young mark closure: address-striped follow_work (ZGC zMark.cpp:635 / zMark.cpp:94-120).
namespace {
constexpr size_t kMarkStripeShift = 20;
constexpr size_t kMarkStripeMultiplier = 4;
constexpr size_t kMarkStripeMax = 64;


// FYS raw workStack.push_back used to skip PushYoungObject recover + StartWho.
// Admit the same host that FYS=0 would have pushed; never enqueue an interior.
BaseObject* AdmitYoungObject(BaseObject* object, const char* origin, const void* slot = nullptr,
                             BaseObject* holder = nullptr)
{
    if (!Heap::IsHeapAddress(object)) {
        return nullptr;
    }

    return object;
}

} // namespace


namespace {
size_t MarkStripeCount(size_t workers)
{
    size_t target = std::max<size_t>(workers * kMarkStripeMultiplier, kMarkStripeMultiplier);
    size_t count = 1;
    while (count < target && count < kMarkStripeMax) {
        count <<= 1;
    }
    return count;
}

} // namespace


class ZMarkTask : public ZRestartableTask {
public:
    explicit ZMarkTask(ZMark* mark, bool partial = false)
        : ZRestartableTask("ZMarkTask"), mark(mark), partial(partial)
    {
        mark->PrepareWork();
    }
    ~ZMarkTask() { mark->FinishWork(); }

    void resize_workers(uint32_t workers) override { mark->ResizeWorkers(workers); }

    void work() override
    {
        SuspendibleThreadSetJoiner stsJoiner;
        mark->FollowWorkComplete(partial);
    }

private:
    ZMark* const mark;
    const bool partial;
};

void ZMark::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
                                          std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                          std::unordered_set<MAddress>& weakSlots,
                                          const std::unordered_set<MAddress>* reachableSlotDomain)
{
    (void)fullYoungScan;
    (void)reachableVec;
    (void)reachableSlots;
    (void)weakSlots;
    (void)reachableSlotDomain;
    (void)workStack;
    const size_t dispelAtEntry = ZPage::GetTdWindowCount();
    ZMark& domain = Heap::GetHeap().young().Mark();
    (void)ZMark::PublishHandshakeMarkWork(workStack, &domain);
    (void)domain.Stacks().Flush(domain.Stripes(), true);
    // ZGC zMark.cpp:944-952: concurrent follow includes termination flush.
    // Mutators can publish after worker termination; only mark-end decides
    // completion, so there is no concurrent stripes-empty assertion here.
    domain.MarkFollow();
    const size_t dispelAtExit = ZPage::GetTdWindowCount();
    CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                 "T-D ghost dispel during striped mark_closure window entry=%zu exit=%zu", dispelAtEntry,
                 dispelAtExit);
}

void ZMark::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
                                   std::vector<BaseObject*>& reachableVec, std::unordered_set<MAddress>& reachableSlots,
                                   std::unordered_set<MAddress>& weakSlots,
                                    const std::unordered_set<MAddress>* reachableSlotDomain)
{
    (void)Heap::GetHeap().young().Mark().Flush(ThreadLocal::GetThreadLocalData());
    if (workStack.empty() && Heap::GetHeap().young().Mark().Stripes().IsEmpty() &&
        Heap::GetHeap().young().Mark().Stacks().IsEmpty()) {
        return;
    }

    TraceYoungClosureStriped(workStack, fullYoungScan, reachableVec, reachableSlots, weakSlots,
                             reachableSlotDomain);
}

bool ZMark::TryEndYoungMark(WorkStack& workStack)
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "young mark-end flush requires stopped mutators");

    (void)ZMark::PublishHandshakeMarkWork(workStack, &Heap::GetHeap().young().Mark());
    const bool ended = Heap::GetHeap().young().Mark().TryEnd();

    if (!ended) {
        return false;
    }
    return true;
}


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
        (void)seed.Flush(domain->Stripes(), true);
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
bool ZMark::MarkEntryObject(BaseObject* obj, const MarkStackEntry& entry,
                            MarkLiveCache* cache)
{
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(obj));
    CHECK_DETAIL(region->IsRelocatable(), "mark consumer requires a relocatable page");
    bool firstLive = entry.inc_live();
    bool already = false;
    if (entry.mark()) {
        already = !region->mark_object(from_object(obj), entry.finalizable(), firstLive);
    }
    if (!already && firstLive) {
        if (cache != nullptr) {
            cache->IncLive(region, obj->GetSize());
        } else {
            region->inc_live(1, obj->GetSize());
        }
    }
    return already;
}


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
    const size_t home = context.StripeId();
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = stacks.StealLocal(victim);
        if (stack != nullptr) {
            stacks.Install(home, stack);
            return true;
        }
    }
    return false;
}

static bool StealGlobalRound(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, size_t workerId,
                             std::atomic<size_t>* stealSuccess, std::atomic<size_t>* stealFailure)
{
    MarkThreadLocalStacks& stacks = context.Stacks();
    const size_t home = context.StripeId();
    for (size_t victim = stripes.Next(home); victim != home; victim = stripes.Next(victim)) {
        MarkStripeStack* stack = stripes.At(victim).StealStack(smr, workerId);
        if (stack != nullptr) {
            if (stealSuccess != nullptr) {
                stealSuccess->fetch_add(1, std::memory_order_relaxed);
            }
            stacks.Install(home, stack);
            return true;
        }
        if (stealFailure != nullptr) {
            stealFailure->fetch_add(1, std::memory_order_relaxed);
        }
    }
    return false;
}

static bool RebalanceWork(MarkContext& context, MarkStripeSet& stripes, MarkTerminate& terminate, size_t workerId,
                          size_t nworkers, ZMark* domain)
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
    const size_t stripe = stripes.StripeForWorker(nworkers, workerId);
    if (context.StripeId() != stripe) {
        context.SetStripeId(stripe);
        (void)context.Stacks().Flush(stripes, true);
    } else if (!terminate.Saturated()) {
        (void)context.Stacks().Flush(stripes, true);
    }
    return domain != nullptr && domain->PollStop();
}

static bool Drain(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes, MarkTerminate& terminate,
                  size_t workerId, size_t nworkers, const ZMark::Process& process, ZMark* domain)
{
    MarkStackEntry entry;
    size_t processed = 0;
    context.SetStripeId(stripes.StripeForWorker(nworkers, workerId));
    context.SetNStripes(stripes.NStripes());
    while (context.Stacks().Pop(smr, workerId, stripes, context.StripeId(), entry)) {
        process(entry);
        if ((processed++ & 31) == 0 && RebalanceWork(context, stripes, terminate, workerId, nworkers, domain)) {
            return false;
        }
    }
    return true;
}

ZMark::Result ZMark::FollowWork(MarkContext& context, MarkingSMR& smr, MarkStripeSet& stripes,
                                          MarkTerminate& terminate, size_t workerId, bool partial,
                                          const Process& process, std::atomic<size_t>* stealSuccess,
                                          std::atomic<size_t>* stealFailure, ZMark* domain)
{
    const size_t nworkers = terminate.workerCount;
    for (;;) {
        if (!Drain(context, smr, stripes, terminate, workerId, nworkers, process, domain)) {
            terminate.Leave();
            return Result::Aborted;
        }
        if (StealLocalRound(context, stripes) ||
            StealGlobalRound(context, smr, stripes, workerId, stealSuccess, stealFailure)) {
            continue;
        }
        if (partial) {
            return Result::Partial;
        }
        if (domain != nullptr && domain->TryProactiveFlush(workerId)) {
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

void ZMark::EnsureWorkers(size_t workers)
{
    CHECK_DETAIL(workers <= ConcGCThreads, "mark workers exceed per-worker storage capacity");
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
    EnsureWorkers(nworkers);
    terminate.Reset(nworkers);
    // zMark.cpp:118-123: stripe count goes to the generation's mark account.
    const ZGenerationId statId =
        generation == MarkingStacks::MarkingGeneration::YOUNG ? ZGenerationId::young : ZGenerationId::old;
    Heap::GetHeap().GetZGeneration(statId).StatMark()->AtMarkStart(targetNStripes);
}

void ZMark::PrepareWork()
{
    CHECK_DETAIL(nworkers != 0, "mark domain needs a worker");
    targetNStripes = CalculateNStripes(nworkers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(nworkers);
    terminate.Reset(nworkers);
    workNProactiveFlush.store(0, std::memory_order_relaxed);
    workNTerminateFlush.store(0, std::memory_order_relaxed);
    terminate.SetResurrected(false);
}

void ZMark::PrepareWork(size_t workers)
{
    nworkers = workers;
    PrepareWork();
}

void ZMark::FollowWorkComplete(bool partial)
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    (void)FollowWork(local, smr, stripes, terminate, workerId, partial,
                     [this, &local](const MarkStackEntry& entry) { MarkAndFollow(local, entry); },
                     nullptr, nullptr, this);
    (void)local.Stacks().Flush(stripes, true);
    local.Cache().Flush();

    ThreadLocal::FlushCurrentThreadMarkStacks();
}

bool ZMark::FollowWorkPartial()
{
    const uint32_t workerId = WorkerThread::worker_id();
    MarkContext local(nworkers, workerId, stripes, Stacks());
    const Result result = FollowWork(local, smr, stripes, terminate, workerId, true,
                     [this, &local](const MarkStackEntry& entry) { MarkAndFollow(local, entry); },
                     nullptr, nullptr, this);
    (void)local.Stacks().Flush(stripes, true);
    local.Cache().Flush();
    return result != Result::Aborted;
}

void ZMark::MarkFollow(bool partial)
{
    for (;;) {
        ZMarkTask task(this, partial);
        gcWorkers->run(&task);
        if (ZAbort::should_abort() || !TryTerminateFlush()) {
            break;
        }
    }
}

void ZMark::MarkAndFollow(MarkContext& ctx, const MarkStackEntry& entry)
{
    if (generation == MarkingStacks::MarkingGeneration::YOUNG) {
        auto visitSlot = [](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnYoungOopField(field);
        };
        auto publish = [this, &ctx](const MarkStackEntry& work) {
            MAddress address = 0;
            if (work.partial_array()) {
                size_t length = 0;
                MarkPartialArray::Decode(work, address, length);
            } else {
                address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(work.object_address()))));
            }
            const size_t stripeIndex = stripes.StripeForAddress(address);
            const bool published = stripeIndex != ctx.StripeId();
            ctx.Stacks().Push(stripes, stripeIndex, work, published);
            if (published) {
                terminate.Wake();
            }
        };
        if (entry.partial_array()) {
            MarkPartialArray::FollowPartialReferences(entry, visitSlot, publish);
            return;
        }
        BaseObject* object = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        if (!Heap::IsHeapAddress(object)) {
            return;
        }
        ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
        if (!region->IsYoungRegion()) {
            return;
        }
        const bool wasMarked = MarkEntryObject(object, entry, &ctx.Cache());
        if (entry.mark() && wasMarked) {
            return;
        }
        if (!object->HasRefField() || !entry.follow()) {
            return;
        }
        FollowObjectReferences(object, entry.finalizable(), visitSlot, publish);
        return;
    }
    auto publish = [this, &ctx](const MarkStackEntry& work) {
        MAddress address = 0;
        if (work.partial_array()) {
            size_t length = 0;
            MarkPartialArray::Decode(work, address, length);
        } else {
            address = reinterpret_cast<MAddress>(to_object(ZOffset::address(to_zoffset(work.object_address()))));
        }
        const size_t stripeIndex = stripes.StripeForAddress(address);
        const bool published = stripeIndex != ctx.StripeId();
        ctx.Stacks().Push(stripes, stripeIndex, work, published);
        if (published) {
            terminate.Wake();
        }
    };
    if (UNLIKELY(MarkPartialArray::IsPartialArrayEntry(entry))) {
        MarkPartialArray::FollowPartialReferences(entry, [&entry](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnOldOopField(field, entry.finalizable());
        }, publish);
        return;
    }
    BaseObject* obj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
    const bool wasMarked = MarkEntryObject(obj, entry, &ctx.Cache());
    if ((!entry.mark() || !wasMarked) && entry.follow()) {
        if (!obj->HasRefField()) {
            return;
        }
        auto visitSlot = [obj, &entry](MAddress slot) {
            auto& field = HeapSlotAt<>(slot);
            ZBarrier::MarkBarrierOnOldOopField(field, entry.finalizable());
        };
        FollowObjectReferences(obj, entry.finalizable(), visitSlot, publish);
    }
}

void ZMark::ResizeWorkers(size_t workers)
{
    CHECK_DETAIL(workers != 0, "mark domain needs a worker");
    nworkers = workers;
    targetNStripes = CalculateNStripes(workers);
    stripes.SetNStripes(targetNStripes);
    EnsureWorkers(workers);
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
bool HeapMarkReady()
{
    return true;
}

bool FlushTargetGCData(ThreadGCData& data, ZMark* domain)
{
    data.storeBarrierBuffer->Flush();
    if (!HeapMarkReady()) {
        return domain != nullptr ? data.FlushMarkStacks(*domain) : false;
    }
    return domain == nullptr ? ZMark::FlushGCDataMarkProducers(data)
                             : ZMark::FlushGCDataMarkProducers(data, domain);
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
    if (!HeapMarkReady()) {
        return published;
    }
    return (domain == nullptr ? ZMark::FlushThreadMarkProducers(tls)
                              : ZMark::FlushThreadMarkProducers(tls, domain)) || published;
}

bool ZMark::HandshakeFlush(ZMark* domain)
{
    auto& manager = MutatorManager::Instance();
    bool flushed = false;
    if (manager.WorldStopped()) {
        {
            std::lock_guard<std::mutex> lock(manager.markFlushThreadMutex);
            for (auto& entry : manager.markFlushThreads) {
                if (entry.second->bufferLive.load(std::memory_order_acquire) == 0) {
                    continue;
                }
                if (FlushThreadLocal(entry.first, domain)) {
                    flushed = true;
                }
            }
        }
        ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator*, ThreadLocalData*) {
            flushed = FlushTargetGCData(data, domain) || flushed;
        });
        flushed = FlushThreadLocal(ThreadLocal::GetThreadLocalData(), domain) || flushed;
        return flushed;
    }

    class ZMarkFlushStacksHandshakeClosure : public HandshakeClosure {
    public:
        explicit ZMarkFlushStacksHandshakeClosure(ZMark* d)
            : HandshakeClosure("ZMarkFlushStacks"), domain_(d), flushed_(false) {}
        void do_thread(ThreadLocalData* tls) override
        {
            if (FlushThreadLocal(tls, domain_)) {
                flushed_ = true;
            }
        }
        bool flushed() const { return flushed_.load(std::memory_order_relaxed); }
    private:
        ZMark* domain_;
        std::atomic<bool> flushed_;
    } cl(domain);
    if (HeapMarkReady()) {
        Heap::GetHeap().GetFinalizerProcessor().Notify();
        Handshake::execute(&cl);
    } else {
        cl.do_thread(ThreadLocal::GetThreadLocalData());
    }
    ThreadGCData::VisitOwners([&](ThreadGCData& data, Mutator* target, ThreadLocalData*) {
        if (target == nullptr) { return; }
        target->MutatorLock();
        if (target->InSaferegion()) {
            flushed = FlushTargetGCData(data, domain) || flushed;
        }
        target->MutatorUnlock();
    });
    flushed = FlushThreadLocal(ThreadLocal::GetThreadLocalData(), domain) || flushed;
    if (cl.flushed()) {
        flushed = true;
    }
    {
        std::lock_guard<std::mutex> lock(manager.markFlushThreadMutex);
        for (auto it = manager.markFlushThreads.begin(); it != manager.markFlushThreads.end();) {
            if (it->second->dying.load(std::memory_order_acquire) != 0 &&
                it->second->refs.load(std::memory_order_acquire) == 0) {
                it = manager.markFlushThreads.erase(it);
            } else {
                ++it;
            }
        }
    }
    return flushed;
}

bool ZMark::Flush()
{
    return HandshakeFlush(this);
}

// ZGC zMark.cpp:998-1004: buffer processing may produce more stack work.
bool ZMark::Flush(ThreadGCData& data)
{
    data.storeBarrierBuffer->Flush();
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
    if (workerId != 0 || workNProactiveFlush.load(std::memory_order_relaxed) == proactiveFlushMax) {
        return false;
    }
    workNProactiveFlush.fetch_add(1, std::memory_order_relaxed);
    return Flush() || !stripes.IsEmpty();
}

bool ZMark::TryTerminateFlush()
{
    terminate.SetResurrected(false);
    workNTerminateFlush.fetch_add(1, std::memory_order_relaxed);
    if (ZVerifyMarking) { verify_worker_stacks_empty(); }
    (void)Flush();
    return !stripes.IsEmpty() || terminate.Resurrected();
}

bool ZMark::TryEnd()
{
    if (terminate.Resurrected()) {
        return false;
    }
    (void)Flush(ThreadLocal::GetThreadLocalData());
    // zMark.cpp:954-970: resurrected, then non-Java flush; empty stripes => complete.
    if (!HeapMarkReady()) {
        return stripes.IsEmpty();
    }
    (void)HandshakeFlush(this);
    (void)FlushStacks();
    if (!stripes.IsEmpty()) {
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

// ZGC zMark.cpp:208-263 follow_array_elements: small arrays are visited
// locally, large arrays publish their aligned middle and trailing parts as
// partial-array entries and follow the leading part locally.
void FollowElements(MAddress start, size_t length, bool finalizable,
                    const FieldVisitor& visit, const EntryPublisher& publish)
{
    if (length <= MIN_LENGTH) {
        for (size_t i = 0; i < length; ++i) {
            visit(start + i * sizeof(MAddress));
        }
        return;
    }
    const MAddress end = start + length * sizeof(MAddress);
    const MAddress middleStart = AlignUp(start + sizeof(MAddress), MIN_SIZE);
    const size_t middleLength = AlignDown((end - middleStart) / sizeof(MAddress), MIN_LENGTH);
    const MAddress middleEnd = middleStart + middleLength * sizeof(MAddress);
    auto push = [&](MAddress address, size_t count) {
        publish(Encode(reinterpret_cast<const void*>(address), count, finalizable));
    };
    if (end > middleEnd) {
        push(middleEnd, (end - middleEnd) / sizeof(MAddress));
    }
    MAddress part = middleEnd;
    while (part > middleStart) {
        const size_t count = AlignUp((part - middleStart) / sizeof(MAddress) / 2, MIN_LENGTH);
        part -= count * sizeof(MAddress);
        push(part, count);
    }
    for (MAddress field = start; field < middleStart; field += sizeof(MAddress)) {
        visit(field);
    }
}

} // namespace MarkPartialArray

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

void ZMark::FollowObjectReferences(BaseObject* object, bool finalizable,
                            const MarkPartialArray::FieldVisitor& visit, const MarkPartialArray::EntryPublisher& publish)
{
    if (object->GetTypeInfo()->IsRawArray()) {
        MArray* array = reinterpret_cast<MArray*>(object);
        TypeInfo* component = array->GetComponentTypeInfo();
        if (component->IsObjectType() || component->IsArrayType() || component->IsInterface()) {
            // zMark.cpp:346-368: array following does not contain a safe
            // iterator split. Invisible roots carry DontFollow upstream.
            MarkPartialArray::FollowElements(reinterpret_cast<MAddress>(array->ConvertToCArray()), array->GetLength(), finalizable, visit, publish);
            return;
        }
    }
    follow_object(object, finalizable);
}

namespace MarkPartialArray {
void FollowPartialReferences(const MarkStackEntry& entry,
                             const FieldVisitor& visit, const EntryPublisher& publish)
{
    MAddress start = 0;
    size_t length = 0;
    Decode(entry, start, length);
    FollowElements(start, length, entry.finalizable(), visit, publish);
}

}
}

namespace MapleRuntime {

}

#include "Heap/z/zMark.inline.hpp"

