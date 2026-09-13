// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "Base/Log.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zCollectedHeap.hpp"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_fwdToGateRefuse{ 0 };
std::atomic<bool> g_fwdToGateAtexit{ false };

} // namespace

void NoteFwdToGateRefuse(const char* site, BaseObject* toObj)
{
    const size_t n = g_fwdToGateRefuse.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_fwdToGateAtexit.exchange(true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr, "[GCV2][fwd-to-gate] atexit refuse=%zu\n",
                         g_fwdToGateRefuse.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
    if (n <= 8 || (n & (n - 1)) == 0) {
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        LOG(RTLOG_ERROR, "[GCV2][fwd-to-gate] refuse n=%zu site=%s to=%p phase=%s", n, site,
            static_cast<void*>(toObj), Collector::GetGCPhaseName(phase));
    }
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"

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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/WCollector/WCollectorInternal.h"

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
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uintptr_t> g_remapYoungRootsTargetSlot{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsBefore{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsAfter{ 0 };
std::atomic<uintptr_t> g_remapYoungRootsResolvedAddress{ 0 };
std::atomic<uint64_t> g_remapYoungRootsVisits{ 0 };
std::atomic<uint64_t> g_remapYoungRootsHeals{ 0 };
std::atomic<bool> g_remapYoungRootsStoreGoodAfter{ false };
} // namespace

void ResetRemapYoungRootsTestReceipt(uintptr_t targetSlot)
{
    g_remapYoungRootsTargetSlot.store(targetSlot, std::memory_order_relaxed);
    g_remapYoungRootsBefore.store(0, std::memory_order_relaxed);
    g_remapYoungRootsAfter.store(0, std::memory_order_relaxed);
    g_remapYoungRootsResolvedAddress.store(0, std::memory_order_relaxed);
    g_remapYoungRootsVisits.store(0, std::memory_order_relaxed);
    g_remapYoungRootsHeals.store(0, std::memory_order_relaxed);
    g_remapYoungRootsStoreGoodAfter.store(false, std::memory_order_relaxed);
}

RemapYoungRootsTestReceipt ReadRemapYoungRootsTestReceipt()
{
    return { g_remapYoungRootsTargetSlot.load(std::memory_order_relaxed),
             g_remapYoungRootsBefore.load(std::memory_order_relaxed),
             g_remapYoungRootsAfter.load(std::memory_order_relaxed),
             g_remapYoungRootsResolvedAddress.load(std::memory_order_relaxed),
             g_remapYoungRootsVisits.load(std::memory_order_relaxed),
             g_remapYoungRootsHeals.load(std::memory_order_relaxed),
             g_remapYoungRootsStoreGoodAfter.load(std::memory_order_relaxed) };
}

void NoteRemapYoungRootsTestReceipt(RefField<>& field, uintptr_t before, bool healed,
                                           bool storeGoodAfter)
{
    const uintptr_t slot = reinterpret_cast<uintptr_t>(&field);
    if (slot != g_remapYoungRootsTargetSlot.load(std::memory_order_relaxed)) {
        return;
    }
    g_remapYoungRootsBefore.store(before, std::memory_order_relaxed);
    g_remapYoungRootsAfter.store(raw(field.GetFieldValue()), std::memory_order_relaxed);
    g_remapYoungRootsResolvedAddress.store(
        reinterpret_cast<uintptr_t>(to_object(field.GetTargetObject())), std::memory_order_relaxed);
    g_remapYoungRootsVisits.fetch_add(1, std::memory_order_relaxed);
    if (healed) {
        g_remapYoungRootsHeals.fetch_add(1, std::memory_order_relaxed);
    }
    g_remapYoungRootsStoreGoodAfter.store(storeGoodAfter, std::memory_order_relaxed);
}
#endif
}

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/StringDedup.h"
#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"

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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/WCollector/WCollectorInternal.h"

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
#if defined(MRT_TESTABLE_INTERNALS)
// Scheduling only: install a barrier after flip, at wait entry, and before
// post-copy cleanup. The callback never supplies a forwarding answer.
using RemapWindowTestHook = void (*)(unsigned, RegionInfo*, BaseObject*);
static std::atomic<RemapWindowTestHook> g_remapWindowTestHook{ nullptr };
extern "C" MRT_EXPORT void MRT_SetRemapWindowTestHook(RemapWindowTestHook hook)
{
    g_remapWindowTestHook.store(hook, std::memory_order_release);
}
void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object)
{
    auto hook = g_remapWindowTestHook.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(point, region, object);
    }
}
#endif
}
