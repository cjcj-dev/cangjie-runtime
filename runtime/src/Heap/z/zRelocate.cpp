// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zAbort.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/shared/stringdedup/stringDedup.hpp"
#include "Heap/z/zMark.hpp"
#include <array>
#include <cassert>
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
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "Heap/z/zStackWatermark.hpp"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zRemembered.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include <cmath>
#include <sched.h>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif
#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"
#include <chrono>
#include "Heap/z/zPage.hpp"
#include "Base/Log.h"
#include "Heap/z/zGeneration.hpp"


namespace MapleRuntime {
static const ZStatCriticalPhase ZCriticalPhaseRelocationStall("Relocation Stall");

// ZGC zRelocate.cpp:1051-1078: claim each thread once across the workers.
class ZRelocateStoreBufferInstallBasePointersThreadClosure {
public:
    void do_thread(Mutator& mutator)
    {
        mutator.GetGCData().storeBarrierBuffer->install_base_pointers();
    }
};

class ZRelocateStoreBufferInstallBasePointersTask final : public ZTask {
    JavaThreadsIterator threads;
public:
    explicit ZRelocateStoreBufferInstallBasePointersTask(ZGeneration* generation)
        : ZTask("ZRelocateStoreBufferInstallBasePointersTask"), threads(generation->id_optional()) {}

    void work() override
    {
        ZRelocateStoreBufferInstallBasePointersThreadClosure closure;
        threads.Apply([&](Mutator& mutator) { closure.do_thread(mutator); });
    }
};

bool ZRelocate::IsFromObject(BaseObject* obj)
    {
        if (!Heap::IsHeapAddress(obj)) {
            return false;
        }
        const MAddress addr = reinterpret_cast<MAddress>(obj);
        return Heap::GetHeap().GetZGeneration(Generation::Young).forwarding_table().get(addr) != nullptr ||
               Heap::GetHeap().GetZGeneration(Generation::Old).forwarding_table().get(addr) != nullptr;
    }

void ZRelocate::StartRelocationTasks(ZGenerationId generation)
{
    ZWorkers& workers = *Heap::GetHeap().GetZGeneration(generation).Workers();
    auto& queue = *Heap::GetHeap().GetZGeneration(generation).relocate().queue();
    CHECK(!queue.is_active());
    queue.activate(workers.active_workers());
}

// ZGC zRelocate.cpp:350-352.
void ZRelocate::add_remset(volatile zpointer* p)
{
    ZGeneration::young()->remember(p);
}

// ZGC zRelocate.cpp:1227-1255.
static void RemapAndMaybeAddRemset(RefField<>& field)
{
    volatile zpointer* const p = reinterpret_cast<volatile zpointer*>(&field);
    const zpointer ptr = field.GetFieldValue();
    if (ZPointer::is_store_good(ptr)) {
        // ZGC zRelocate.cpp:1230-1232: store-good already has a remset entry.
        return;
    }
    const zaddress address = ZBarrier::load_barrier_on_oop_field_preloaded(p, ptr);
    if (is_null(address)) {
        return;
    }
    if (Heap::is_old(untype(address))) {
        return;
    }
    ZRelocate::add_remset(p);
}

class ZRelocateAddRemsetForFlipPromoted final : public ZRestartableTask {
public:
    explicit ZRelocateAddRemsetForFlipPromoted(ZArray<ZPage*>* pages)
        : ZRestartableTask("ZRelocateAddRemsetForFlipPromoted"), iter(pages) {}
    void work() override
    {
        SuspendibleThreadSetJoiner stsJoiner;
        for (ZPage* page; iter.next(&page);) {
            page->object_iterate([&](BaseObject* object) {
                ZIterator::basic_oop_iterate_safe(object, object->GetTypeInfo(), RemapAndMaybeAddRemset);
            });
            SuspendibleThreadSet::yield();
            if (ZGeneration::young()->Workers()->should_worker_resize()) {
                return;
            }
        }
    }
private:
    ZArrayParallelIterator<ZPage*> iter;
};

// ZGC zRelocate.cpp:1289: both generations submit their installed set.
void ZRelocate::relocate(ZRelocationSet* relocation_set)
{
    CHECK(relocation_set->generation() == generation);
    auto& manager = Heap::GetHeap().page_allocator();
    ZWorkers& workers = *generation->Workers();
    {
        // ZGC zRelocate.cpp:1289-1296: preserve object starts before page
        // relocation destroys the liveness information used to find them.
        ZRelocateStoreBufferInstallBasePointersTask bufferTask(generation);
        workers.run(&bufferTask);
    }
    if (generation->is_young()) {
        ForwardTask<Generation::Young> task(manager, relocation_set);
        workers.run(&task);
    } else {
        ForwardTask<Generation::Old> task(manager, relocation_set);
        workers.run(&task);
    }
    if (relocation_set->generation()->is_young()) {
        ZRelocateAddRemsetForFlipPromoted task(relocation_set->flip_promoted_pages());
        workers.run(&task);
    }
