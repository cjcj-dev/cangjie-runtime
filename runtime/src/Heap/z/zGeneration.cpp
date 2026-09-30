// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.inline.hpp"
#include "Heap/z/zIterator.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Mutator/Mutator.inline.h"
#include "Heap/z/zAbort.inline.hpp"
#include "Heap/z/zBreakpoint.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zResurrection.hpp"
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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include "Concurrency/Concurrency.h"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zRelocate.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Common/SuspendibleThreadSet.h"
#include "Sync/Sync.h"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zStackWatermark.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "Mutator/Handshake.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"

#include "Heap/z/z_globals.hpp"
namespace MapleRuntime {
// ZGC zGeneration.cpp:161-163.
void ZGeneration::mark_flush(ThreadGCData& data)
{
    mark->Flush(data);
}


// ZGC zGeneration.cpp:69-76: one generation timer per young collection type.
static const ZStatPhaseGeneration ZPhaseGenerationYoung[] {
    {"Young Generation", ZGenerationId::young},
    {"Young Generation (Promote All)", ZGenerationId::young},
    {"Young Generation (Collect Roots)", ZGenerationId::young},
    {"Young Generation", ZGenerationId::young}
};
static const ZStatPhaseGeneration ZPhaseGenerationOld("Old Generation", ZGenerationId::old);

// ZGC zGeneration.cpp:78-98: phase identity is selected at the VM operation or concurrent entry.
static const ZStatPhasePause ZPhasePauseMarkStartYoung("Pause Mark Start", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkStartYoungAndOld("Pause Mark Start (Major)", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkEndYoung("Pause Mark End", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseRelocateStartYoung("Pause Relocate Start", ZGenerationId::young);
static const ZStatPhasePause ZPhasePauseMarkEndOld("Pause Mark End", ZGenerationId::old);
static const ZStatPhasePause ZPhasePauseRelocateStartOld("Pause Relocate Start", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkYoung("Concurrent Mark", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkContinueYoung("Concurrent Mark Continue", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkFreeYoung("Concurrent Mark Free", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentResetRelocationSetYoung("Concurrent Reset Relocation Set", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentSelectRelocationSetYoung("Concurrent Select Relocation Set", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentRelocateYoung("Concurrent Relocate", ZGenerationId::young);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkOld("Concurrent Mark", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkContinueOld("Concurrent Mark Continue", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentMarkFreeOld("Concurrent Mark Free", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentResetRelocationSetOld("Concurrent Reset Relocation Set", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentSelectRelocationSetOld("Concurrent Select Relocation Set", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentRelocateOld("Concurrent Relocate", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentProcessNonStrongOld("Concurrent Process Non-Strong", ZGenerationId::old);
static const ZStatPhaseConcurrent ZPhaseConcurrentRemapRootsOld("Concurrent Remap Roots", ZGenerationId::old);

static const ZStatSubPhase ZSubPhaseConcurrentMarkFollowYoung("Concurrent Mark Follow", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentMarkRootsYoung("Concurrent Mark Roots", ZGenerationId::young);
static const ZStatSubPhase ZSubPhaseConcurrentMarkRootsOld("Concurrent Mark Roots", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentMarkFollowOld("Concurrent Mark Follow", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRootsColoredOld("Concurrent Remap Roots Colored", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRootsUncoloredOld("Concurrent Remap Roots Uncolored", ZGenerationId::old);
static const ZStatSubPhase ZSubPhaseConcurrentRemapRememberedOld("Concurrent Remap Remembered", ZGenerationId::old);
ZGenerationYoung* ZGeneration::_young = nullptr;
ZGenerationOld* ZGeneration::_old = nullptr;

ZGenerationYoung::ZGenerationYoung(ZPageTable* page_table, const ZForwardingTable* old_forwarding_table,
                                     RegionManager* page_allocator)
    : ZGeneration(ZGenerationId::young), _remembered(page_table, old_forwarding_table, page_allocator)
{
    _young = this;
}
ZGenerationOld::ZGenerationOld() : ZGeneration(ZGenerationId::old)
{
    _old = this;
}

ZGenerationId ZGeneration::id() const { return _id; }

ZGenerationIdOptional ZGeneration::id_optional() const
{
    return static_cast<ZGenerationIdOptional>(id());
}

bool ZGeneration::is_young() const { return id() == ZGenerationId::young; }
bool ZGeneration::is_old() const { return id() == ZGenerationId::old; }
ZGenerationYoung* ZGeneration::young() { return _young; }
ZGenerationOld* ZGeneration::old() { return _old; }
ZGeneration* ZGeneration::generation(ZGenerationId id)
{
    return id == ZGenerationId::young ? static_cast<ZGeneration*>(_young) : static_cast<ZGeneration*>(_old);
}

ZGeneration::ZGeneration(ZGenerationId generation)
    : mark(std::make_unique<ZMark>(ZMarkStripesMax,
          generation == ZGenerationId::young ? MarkingStacks::MarkingGeneration::YOUNG
                                                 : MarkingStacks::MarkingGeneration::MAJOR)),
      _id(generation == ZGenerationId::young ? ZGenerationId::young : ZGenerationId::old),
      statHeap(),
      _relocation_set(this),
      _relocate(std::make_unique<ZRelocate>(this))
{
    ZJNICritical::initialize();
}

ZGeneration::~ZGeneration()
{
    StopWorkers();
}

// ZGC zGeneration.cpp:197-207: select policy at the generation boundary.
static double fragmentation_limit(ZGenerationId generation)
{
    if (generation == ZGenerationId::old) {
        return ZFragmentationLimit;
    } else {
        return ZYoungCompactionLimit;
    }
}
double ZGeneration::FragmentationLimit() const
{
    return fragmentation_limit(_id);
}



bool ZGenerationOld::active_remset_is_current() const
