// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRelocationSet.hpp"
#include "Heap/z/zRelocate.hpp"

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
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zForwardingEntry.hpp"
#include "Heap/z/zArray.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zRelocate.hpp"


namespace MapleRuntime {

static const ZStatSubPhase PPostTrace("PostTrace", ZGenerationId::old);

void ZGenerationOld::PostTrace()
{
    ZStatTimerOld zstatTimer(PPostTrace);
    // Value-only cycle roots still depend on the preceding relocation receipts.
    // Complete their owner handoff while that authority is queryable.
    // zGeneration.cpp:1261 mark_end does not reset forwarding.
    Heap::GetHeap().cross_vm().PrepareCycleRef(discoveredExternObjects);
}
} // namespace MapleRuntime

namespace MapleRuntime {
void RegionManager::ResetFlipPromotedPages()
{
}
