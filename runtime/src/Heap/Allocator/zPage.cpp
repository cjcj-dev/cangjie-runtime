// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
// enroltime: defined out of line so RegionInfo.h does not have to see Heap/GCPhase.
void RegionInfo::NoteEnrolPhase()
{
    // Diagnostic-only. gc_unit fixtures never Heap::Init, so
    // CollectorProxy::currentCollector is null and GetGCPhase would fault.
    // CollectorResources is always constructed; IsGcStarted is false there.
    if (!Heap::GetHeap().GetCollectorResources().IsGcStarted()) {
        return;
    }
    const GCPhase phase = Heap::GetHeap().GetGCPhase();
    const bool afterFlip = (phase == GCPhase::GC_PHASE_PREFORWARD || phase == GCPhase::GC_PHASE_FORWARD);
    std::atomic<uint64_t>& counter = afterFlip ? EnrolAfterFlip() : EnrolBeforeFlip();
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0) {
        return;
    }
    LOG(RTLOG_ERROR, "[ENROLTIME] afterFlip=%d n=%lu phase=%d before=%lu after=%lu", afterFlip ? 1 : 0, n,
        static_cast<int>(phase), EnrolBeforeFlip().load(std::memory_order_relaxed),
        EnrolAfterFlip().load(std::memory_order_relaxed));
}
} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
std::atomic<RegionInfo::GhostLookupTestHook> RegionInfo::ghostLookupTestHook { nullptr };
std::atomic<size_t> RegionInfo::ghostLookupTestHookCalls { 0 };


void RegionInfo::SetGhostLookupTestHook(GhostLookupTestHook hook)
{
    ghostLookupTestHookCalls.store(0, std::memory_order_relaxed);
    ghostLookupTestHook.store(hook, std::memory_order_release);
}

size_t RegionInfo::GhostLookupTestHookCalls()
{
    return ghostLookupTestHookCalls.load(std::memory_order_acquire);
}

void RegionInfo::RunGhostLookupTestHook(RegionInfo* region)
{
    GhostLookupTestHook hook = ghostLookupTestHook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook != nullptr) {
        ghostLookupTestHookCalls.fetch_add(1, std::memory_order_relaxed);
        hook(region);
    }
}

#endif
}
