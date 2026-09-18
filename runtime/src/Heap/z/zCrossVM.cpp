// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCrossVM.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Common/ScopedObjectAccess.h"

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
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
std::function<void(const ExportOwnershipTestObservation&)> ZCrossVM::testExportOwnershipResult;
#endif
static_assert(sizeof(RefField<false>) == 8, "RefField colour layout must preserve the 64-bit ABI");
std::atomic<size_t> g_forwardRaceTotalCount{ 0 };
std::atomic<size_t> g_forwardRaceStillBadCount{ 0 };

void ReportForwardRaceCounts()
{
}




extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(ZBarrier::ReadReference(this,
            HeapSlotAt<false>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(ZBarrier::ReadReference(this,
            HeapSlotAt<false>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler ZCrossVM::GetCrossRefHandler(BaseObject *foreignProxy)
{
#if defined(MRT_GC_UNIT_TESTS)
    if (cycleRefHandlerForTest != nullptr) {
        return cycleRefHandlerForTest;
    }
#endif
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void ZCrossVM::ResolveCycleRef()
{
#if defined (__OHOS__) || defined(MRT_GC_UNIT_TESTS)
    // Leave saferegion before acquiring either owner. The resolver owner is not
    // used by GC; it preserves the former single-resolver property while the
    // root-carrier owner is released around every managed callback.
    ScopedObjectAccess soa;
    std::unique_lock<std::mutex> resolverLock(cycleResolverMtx, std::try_to_lock);
    if (!resolverLock.owns_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    size_t i = 0;
    std::unique_lock<std::mutex> cycleLock(cycleWorkStackMtx, std::try_to_lock);
    if (!cycleLock.owns_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    const bool enteredInRelocate =
        ZGeneration::old() != nullptr && ZGeneration::old()->is_phase_relocate();
    std::unordered_set<U32> resolvedIds;
    for (;;) {
        auto it = cycleRefWorkStack.begin();
        while (it != cycleRefWorkStack.end()) {
            BaseObject* candidate = it->first;
            U32 candidateId = static_cast<ExportObject*>(candidate)->GetId();
            if (resolvedIds.find(candidateId) != resolvedIds.end()) {
                ++it;
                continue;
            }
            auto& heap = Heap::GetHeap();
            if (!heap.CheckExportObjState(candidateId, candidate) ||
                resurrectedExportObjectes.find(candidate) != resurrectedExportObjectes.end() ||
                resurrectedExportObjectesForwardPhase.find(candidate) !=
                    resurrectedExportObjectesForwardPhase.end()) {
                cycleRefProgress.erase(candidateId);
                it = cycleRefWorkStack.erase(it);
                continue;
            }
            break;
        }
        if (it == cycleRefWorkStack.end()) {
            break;
        }

        static constexpr size_t taskNum = 100;
        if ((!enteredInRelocate && ZGeneration::old() != nullptr &&
             ZGeneration::old()->is_phase_relocate()) || i >= taskNum) {
            cycleLock.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }

        U32 id = static_cast<ExportObject*>(it->first.object)->GetId();
        size_t externIndex = cycleRefProgress[id];
        void* returnUnit = nullptr;
        for (;;) {
            // A GC preforward pass may replace the map key and list elements
            // while the callback is parked. Re-find by stable export id and
            // fetch the current addresses before each managed invocation.
            it = std::find_if(cycleRefWorkStack.begin(), cycleRefWorkStack.end(),
                [id](const auto& entry) {
                    return static_cast<ExportObject*>(entry.first.object)->GetId() == id;
                });
            if (it == cycleRefWorkStack.end() || externIndex >= it->second.size()) {
                break;
            }
            if (!enteredInRelocate && ZGeneration::old() != nullptr &&
                ZGeneration::old()->is_phase_relocate()) {
                cycleLock.unlock();
                CJ_MRT_RolveCycleRef();
                return;
            }
            BaseObject* exportObj = it->first;
            auto externIt = it->second.begin();
            std::advance(externIt, static_cast<ptrdiff_t>(externIndex));
            BaseObject* externObj = *externIt;
            auto resolveHook = GetCrossRefHandler(externObj);

            // ResolveCycleRefStub enters managed code. A late safepoint can
            // park there, so the GC-owned root carrier must be available while
            // the callback runs. cycleResolverMtx alone prevents duplicate
            // resolver delivery and is never acquired by a GC consumer.
            cycleLock.unlock();
#if defined(MRT_GC_UNIT_TESTS)
            // The minimal gc_unit process has no scheduler-owned CJThread for
            // the assembly N2C adapter. The injected handler still runs from
            // this product call site and enters the real HandleSafepoint path;
            // production always takes the adapter below.
            if (cycleRefHandlerForTest != nullptr) {
                resolveHook(exportObj, externObj);
            } else {
                ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
            }
#else
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
#endif
            cycleLock.lock();

            // The callback was delivered while the full entry stayed in the
            // GC-visible carrier. Commit that delivery before observing a
            // phase change so a PREFORWARD repost resumes at the next item
            // instead of delivering this one again.
            ++externIndex;
            cycleRefProgress[id] = externIndex;
        }

        auto& heap = Heap::GetHeap();
        heap.SetExportObjActiveState(id, false);
        cycleRefProgress.erase(id);
        resolvedIds.insert(id);
        ++i;
    }
    cycleLock.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void ZCrossVM::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}


ValueRoot::ValueRoot(BaseObject* value) : ValueRoot(value, ForwardingStage::OverwritePrevious) {}
ValueRoot::ValueRoot(BaseObject* value, ForwardingStage source)
    : object(value), stage(source), color(::g_cjLoadGoodMask),
      generation(source == ForwardingStage::IncomingNew && Heap::IsHeapAddress(value)
          ? Heap::page(reinterpret_cast<MAddress>(value))->GetOwnerGeneration() : Generation::Old) {}
ForwardingStage ValueRoot::Stage() const
{
    const uintptr_t mask = generation == Generation::Young ? ZPointerRemappedYoungMask : ZPointerRemappedOldMask;
    return (ZPointer::remap_bits(color) & mask) != 0 ? stage : ForwardingStage::OverwritePrevious;
}
extern thread_local const char* gMinorRootOrigin;

void ZCrossVM::ResurrectExportObject(BaseObject* obj)
    {
        ZGeneration* generation = Heap::GetHeap().ObjectGeneration(obj) == Generation::Young ?
            static_cast<ZGeneration*>(ZGeneration::young()) : static_cast<ZGeneration*>(ZGeneration::old());
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        if (generation == nullptr || !generation->is_phase_relocate()) {
            resurrectedExportObjectes.erase(obj);
            resurrectedExportObjectes.insert(ValueRoot(ResolveCurrentValueRoot(
                obj, &resurrectedExportObjectes, Heap::GetHeap().ObjectGeneration(obj), ForwardingStage::IncomingNew),
                ForwardingStage::IncomingNew));
        } else {
            resurrectedExportObjectesForwardPhase.erase(obj);
            resurrectedExportObjectesForwardPhase.insert(ValueRoot(
                ResolveCurrentValueRoot(
                    obj, &resurrectedExportObjectesForwardPhase, Heap::GetHeap().ObjectGeneration(obj), ForwardingStage::IncomingNew),
                ForwardingStage::IncomingNew));
        }
    }

void ZCrossVM::PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
        CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
        for (auto& entry : discoveredExternObjects) {
            ValueRootList& destination = cycleRefWorkStack[entry.first];
            destination.splice(destination.end(), entry.second);
        }
        discoveredExternObjects.clear();
    }

void ZCrossVM::MergeResurrectExportObjects(Generation generation)
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, generation);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }

void ZCrossVM::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Young);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Young);
        gMinorRootOrigin = "value_export";
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        gMinorRootOrigin = "value_export_fwd";
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Young);
    gMinorRootOrigin = "value_cycle";
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
    gMinorRootOrigin = "unknown";
}

void ZCrossVM::FindUselessExternObjects()
{
    std::lock_guard<std::mutex> lock(externMtx);
    CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
}

void ZCrossVM::ProcessExportRoots(WorkStack& foreignRootsSet)
{
    while (!foreignRootsSet.empty()) {
        if (ZAbort::should_abort()) {
            return;
        }
        const MarkStackEntry entry = foreignRootsSet.back();
        foreignRootsSet.pop_back();
        BaseObject* exportObj = to_object(ZOffset::address(to_zoffset(entry.object_address())));
        if (exportObj == nullptr) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(externMtx);
            // Multiple export handles may name the same owner.
            if (!discoveredExternObjects.emplace(exportObj, ValueRootList{}).second) {
                continue;
            }
        }
        if (Heap::IsHeapAddress(exportObj)) {
            Heap::GetHeap().old().MarkObjectIfActive<false, true, true, false>(from_object(exportObj));
        }
        ZMark& mark = Heap::GetHeap().old().Mark();
        mark.BindWorkers(Heap::GetHeap().old().Workers());
        (void)mark.Stacks().Flush(mark.Stripes(), true);
        mark.MarkFollow();
        if (!ZAbort::should_abort()) {
            CHECK_DETAIL(mark.Stripes().IsEmpty(),
                         "export closure returned without coordinated worker termination");
        }

        // ZMark::mark_and_follow (zMark.cpp:412-415) deduplicates GC liveness,
        // not ownership. Cangjie's foreign-cycle handoff has no JNI equivalent:
        // every export owner needs its own strong reachable foreign set, even
        // when the young-roots prelude or another owner already marked it.
        std::unordered_set<BaseObject*> visited;
        std::vector<BaseObject*> pending{exportObj};
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (!visited.insert(object).second) {
                continue;
            }
            if (object->GetTypeInfo()->IsForeignType()) {
                std::lock_guard<std::mutex> lock(externMtx);
                discoveredExternObjects[exportObj].push_back(object);
            }
            // Discovery is not keep-alive (zReferenceProcessor.cpp:175-203):
            // do not turn a weak referent into an export ownership edge.
            object->ForEachRefField([&](RefField<>& field) {
                BaseObject* target = Heap::GetHeap().GetCollector().GetAndTryTagObj(HeapGcState::RefSlotKind::STRONG, object, field);
                if (target != nullptr) {
                    pending.push_back(target);
                }
            });
        }
    }
}

BaseObject* ZCrossVM::ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
                                                      ForwardingStage stage) const
{
    if (value == nullptr || !Heap::IsHeapAddress(value)) {
        return value;
    }
    const ForwardingProvenance provenance{
        ForwardingHolderKind::Static, owner, nullptr, stage, ForwardingWriterKind::CollectorHeal,
        ForwardingSourceKind::CallerValue, nullptr, nullptr, ForwardingFieldKind::RootSlot
    };
    // ZUncoloredRoot::make_load_good (zUncoloredRoot.inline.hpp:62-69)
    // preserves load-good identity. IncomingNew carries the caller's current
    // identity; a page owner alone cannot distinguish overlapping from/to keys.
    if (stage == ForwardingStage::IncomingNew) {
        return Heap::GetHeap().GetCollector().ValidateCurrentValue(value, provenance);
    }
    // Stored roots still need remapping using their source page's generation,
    // which can differ from the generation currently visiting the roots.
    (void)generation;
    const auto forwarding = forwarding_for_page(
        Heap::page(reinterpret_cast<MAddress>(value)));
    BaseObject* current = value;
    if (forwarding) {
        const MAddress target = forwarding->find(reinterpret_cast<MAddress>(value));
        current = target != 0 ? reinterpret_cast<BaseObject*>(target)
            : Heap::GetHeap().GetCollector().ResolveStoreValue(value, provenance, static_cast<Generation>(forwarding->table_generation()));
    }
    CHECK_DETAIL(current != nullptr && Heap::IsHeapAddress(current),
                 "value root resolve requires a heap to-address from=%p current=%p", value, current);
    CHECK_DETAIL(HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable,
                 "value root resolve requires a usable target from=%p current=%p", value, current);
    return current;
}

void ZCrossVM::CurrentizeValueRootSet(ValueRootSet& roots, Generation generation) const
{
    ValueRootSet current;
    current.reserve(roots.size());
    for (const ValueRoot& value : roots) {
        current.insert(ValueRoot(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
                                 ForwardingStage::IncomingNew));
    }
    roots.swap(current);
}

void ZCrossVM::CurrentizeValueRootMap(
    ValueRootMap& roots, Generation generation) const
{
    ValueRootMap current;
    current.reserve(roots.size());
    for (const auto& entry : roots) {
        ValueRoot key(ResolveCurrentValueRoot(entry.first, &roots, generation, entry.first.Stage()),
                      ForwardingStage::IncomingNew);
        ValueRootList& values = current[key];
        for (const ValueRoot& value : entry.second) {
            values.emplace_back(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
                                ForwardingStage::IncomingNew);
        }
    }
    roots.swap(current);
}

void ZCrossVM::VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Old);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Old);
        for (BaseObject* obj : resurrectedExportObjectes) {
            visitor(obj);
        }
        for (BaseObject* obj : resurrectedExportObjectesForwardPhase) {
            visitor(obj);
        }
    }
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
    auto it = cycleRefWorkStack.begin();
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        visitor(exportObj);
        for (auto &externObj : it->second) {
            visitor(externObj.object);
        }
        it++;
    }
}

void ZCrossVM::PreforwardDiscoveredExternObjects(Generation generation)
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    CurrentizeValueRootMap(cycleRefWorkStack, generation);
}

void ZCrossVM::PreforwardAllResurrectExportFromObjects(Generation generation)
{
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    CurrentizeValueRootSet(resurrectedExportObjectes, generation);
    CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
}
} // namespace MapleRuntime
