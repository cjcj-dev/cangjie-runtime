// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Common/BaseObject.inline.h"
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
#include "Heap/z/zBarrier.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"

namespace MapleRuntime {
static_assert(sizeof(RefField<false>) == 8, "RefField colour layout must preserve the 64-bit ABI");





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
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void ZCrossVM::ResolveCycleRef()
{
#if defined (__OHOS__)
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
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
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


// Like ZUncoloredRoot's saved color, the carrier records the epoch in which
// its address was made load-good. The consumer, not the page, selects remapping.
ValueRoot::ValueRoot(BaseObject* value) : object(value), color(::g_cjLoadGoodMask) {}

void ZCrossVM::ResurrectExportObject(BaseObject* obj)
    {
        ZGeneration* generation = Heap::GetHeap().ObjectGeneration(obj) == Generation::Young ?
            static_cast<ZGeneration*>(ZGeneration::young()) : static_cast<ZGeneration*>(ZGeneration::old());
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        if (generation == nullptr || !generation->is_phase_relocate()) {
            resurrectedExportObjectes.erase(obj);
            resurrectedExportObjectes.insert(ValueRoot(ResolveCurrentValueRoot(
                ValueRoot(obj))));
        } else {
            resurrectedExportObjectesForwardPhase.erase(obj);
            resurrectedExportObjectesForwardPhase.insert(ValueRoot(
                ResolveCurrentValueRoot(
                    ValueRoot(obj))));
        }
    }

void ZCrossVM::PrepareCycleRef()
    {
        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
        CurrentizeValueRootMap(cycleRefWorkStack);
        CurrentizeValueRootMap(discoveredExternObjects);
        for (auto& entry : discoveredExternObjects) {
            ValueRootList& destination = cycleRefWorkStack[entry.first];
            destination.splice(destination.end(), entry.second);
        }
        discoveredExternObjects.clear();
    }

void ZCrossVM::MergeResurrectExportObjects(Generation generation)
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
            resurrectedExportObjectesForwardPhase.end());
        resurrectedExportObjectesForwardPhase.clear();
    }

void ZCrossVM::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack);
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
}

void ZCrossVM::FindUselessExternObjects()
{
    std::lock_guard<std::mutex> lock(externMtx);
    CurrentizeValueRootMap(discoveredExternObjects);
}

void ZCrossVM::ProcessExportRoots(ValueRootList& exportOwners)
{
    while (!exportOwners.empty()) {
        if (ZAbort::should_abort()) {
            return;
        }
        const ValueRoot owner = exportOwners.back();
        BaseObject* exportObj = ResolveCurrentValueRoot(owner);
        exportOwners.pop_back();
        if (exportObj == nullptr) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(externMtx);
            // Multiple export handles may name the same owner.
            // ZUncoloredRoot::make_load_good (zUncoloredRoot.inline.hpp:62-69):
            // preserve the load-good identity just produced by the root barrier.
            if (!discoveredExternObjects.emplace(
                    ValueRoot(exportObj), ValueRootList{}).second) {
                continue;
            }
        }
        // GC liveness was published by the original root slot barrier and
        // completed by the generation's normal mark termination. This walk
        // records foreign ownership only; GC deduplication cannot replace it.
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
                discoveredExternObjects[ValueRoot(exportObj)].emplace_back(
                    object);
            }
            // Discovery is not keep-alive (zReferenceProcessor.cpp:175-203):
            // do not turn a weak referent into an export ownership edge.
            object->ForEachRefField([&](RefField<>& field) {
                BaseObject* target = ZBarrier::GetAndTryTagObj(ZBarrier::RefSlotKind::STRONG, object, field);
                if (target != nullptr) {
                    pending.push_back(target);
                }
            });
        }
    }
}

BaseObject* ZCrossVM::ResolveCurrentValueRoot(const ValueRoot& root) const
{
    BaseObject* const value = root.object;
    if (value == nullptr || !Heap::IsHeapAddress(value)) {
        return value;
    }
    // ZUncoloredRoot::make_load_good (zUncoloredRoot.inline.hpp:62-69):
    // the saved root color is the only remap discriminator.
    const zpointer colorPtr = ZAddress::color(zaddress::null, root.color);
    const bool loadGood = ZPointer::is_load_good(colorPtr);
    if (!loadGood) {
        // ZGeneration::relocate_or_remap_object returns the original address
        // when this generation's forwarding table has no entry for it.
        BaseObject* current = ZBarrier::remap_generation(colorPtr)
            ->relocate_or_remap_object(value);
        if (current == nullptr || !Heap::IsHeapAddress(current) ||
            ZBarrier::JudgeHandOutTarget(current) != HandVerdict::Usable) {
            ZBarrier::FailClosedLoad("value root relocate_or_remap requires usable to", value, 0);
        }
        return current;
    }
    return ZBarrier::ValidateCurrentValue(value);
}

void ZCrossVM::CurrentizeValueRootSet(ValueRootSet& roots) const
{
    ValueRootSet current;
    current.reserve(roots.size());
    for (const ValueRoot& value : roots) {
        current.insert(ValueRoot(ResolveCurrentValueRoot(value)));
    }
    roots.swap(current);
}

void ZCrossVM::CurrentizeValueRootMap(
    ValueRootMap& roots) const
{
    ValueRootMap current;
    current.reserve(roots.size());
    for (const auto& entry : roots) {
        ValueRoot key(ResolveCurrentValueRoot(entry.first));
        ValueRootList& values = current[key];
        for (const ValueRoot& value : entry.second) {
            values.emplace_back(ResolveCurrentValueRoot(value));
        }
    }
    roots.swap(current);
}

void ZCrossVM::VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lg(resurrectExportMtx);
        CurrentizeValueRootSet(resurrectedExportObjectes);
        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
        for (BaseObject* obj : resurrectedExportObjectes) {
            visitor(obj);
        }
        for (BaseObject* obj : resurrectedExportObjectesForwardPhase) {
            visitor(obj);
        }
    }
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CurrentizeValueRootMap(cycleRefWorkStack);
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
    CurrentizeValueRootMap(cycleRefWorkStack);
}

void ZCrossVM::PreforwardAllResurrectExportFromObjects(Generation generation)
{
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    CurrentizeValueRootSet(resurrectedExportObjectes);
    CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase);
}
} // namespace MapleRuntime

namespace MapleRuntime {

}
