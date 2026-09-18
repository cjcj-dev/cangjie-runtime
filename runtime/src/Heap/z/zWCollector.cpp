// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zMark.hpp"

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
#include "Heap/z/zCollectorInternal.hpp"

namespace MapleRuntime {
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

CrossRefHandler CopyCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
#if defined(MRT_GC_UNIT_TESTS)
    if (cycleRefHandlerForTest != nullptr) {
        return cycleRefHandlerForTest;
    }
#endif
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void CopyCollector::ResolveCycleRef()
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
void CopyCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}

bool CopyCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
