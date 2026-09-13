// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/WCollector/WCollector.h"
#include "Heap/WCollector/RememberedHolderPolicy.h"

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
#include "Heap/Collector/MarkPartialArray.h"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zWorkers.hpp"
#include "Heap/Verify/TraceClear.h"
#include "Heap/z/zMark.hpp"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NwDropAudit.h"
#include "Heap/Verify/GarbRegionDiag.h"
#include "Heap/Verify/Stw2CurrentAudit.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "UnwindStack/StackFrameCursor.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "Heap/WCollector/WCollectorInternal.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<uint64_t> g_youngWeakSerialDiscoveries{ 0 };
std::atomic<uint64_t> g_youngWeakLegacyParallelDiscoveries{ 0 };
std::atomic<uint64_t> g_youngWeakStripedDiscoveries{ 0 };
} // namespace

void ResetYoungWeakClosureTestReceipt()
{
    g_youngWeakSerialDiscoveries.store(0, std::memory_order_relaxed);
    g_youngWeakLegacyParallelDiscoveries.store(0, std::memory_order_relaxed);
    g_youngWeakStripedDiscoveries.store(0, std::memory_order_relaxed);
}

void NoteYoungWeakClosureDiscovery(YoungWeakClosureVariant variant)
{
    switch (variant) {
        case YoungWeakClosureVariant::SERIAL:
            g_youngWeakSerialDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
        case YoungWeakClosureVariant::LEGACY_PARALLEL:
            g_youngWeakLegacyParallelDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
        case YoungWeakClosureVariant::STRIPED:
            g_youngWeakStripedDiscoveries.fetch_add(1, std::memory_order_relaxed);
            return;
    }
}

YoungWeakClosureTestReceipt ReadYoungWeakClosureTestReceipt()
{
    return { g_youngWeakSerialDiscoveries.load(std::memory_order_relaxed),
             g_youngWeakLegacyParallelDiscoveries.load(std::memory_order_relaxed),
             g_youngWeakStripedDiscoveries.load(std::memory_order_relaxed) };
}
#endif
}
