// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zVerify.hpp"
#include "Heap/WCollector/WCollector.h"

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
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zForwarding.hpp"
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
struct RemsetFilterReceiptState {
    std::atomic<uint64_t> seen { 0 };
    std::atomic<uint64_t> consumed { 0 };
    std::atomic<uint64_t> stale { 0 };
    std::atomic<uint64_t> deadHolder { 0 };
    std::atomic<uint64_t> noOrigin { 0 };
    std::atomic<uint64_t> badTarget { 0 };
    std::atomic<MAddress> lastConsumedSlot { 0 };
    std::atomic<MAddress> lastStaleSlot { 0 };
    std::atomic<MAddress> lastDeadHolderSlot { 0 };
    std::atomic<MAddress> lastNoOriginSlot { 0 };
    std::atomic<MAddress> lastBadTargetSlot { 0 };
};
RemsetFilterReceiptState g_remsetFilterReceipt;
}

void ResetRemsetFilterTestReceipt()
{
    g_remsetFilterReceipt.seen.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.consumed.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.stale.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.deadHolder.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.noOrigin.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.badTarget.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastConsumedSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastStaleSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastDeadHolderSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastNoOriginSlot.store(0, std::memory_order_relaxed);
    g_remsetFilterReceipt.lastBadTargetSlot.store(0, std::memory_order_relaxed);
}

RemsetFilterTestReceipt ReadRemsetFilterTestReceipt()
{
    return { g_remsetFilterReceipt.seen.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.consumed.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.stale.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.deadHolder.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.noOrigin.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.badTarget.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastConsumedSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastStaleSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastDeadHolderSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastNoOriginSlot.load(std::memory_order_relaxed),
             g_remsetFilterReceipt.lastBadTargetSlot.load(std::memory_order_relaxed) };
}

void NoteRemsetFilterTestReceipt(MAddress slot, RemsetFilterReceiptReason reason, bool consumed)
{
    if (slot == 0) {
        return;
    }
    g_remsetFilterReceipt.seen.fetch_add(1, std::memory_order_relaxed);
    if (consumed) {
        g_remsetFilterReceipt.consumed.fetch_add(1, std::memory_order_relaxed);
        g_remsetFilterReceipt.lastConsumedSlot.store(slot, std::memory_order_relaxed);
    }
    switch (reason) {
        case RemsetFilterReceiptReason::kStale:
            g_remsetFilterReceipt.stale.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastStaleSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kDeadHolder:
            g_remsetFilterReceipt.deadHolder.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastDeadHolderSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNoOrigin:
            g_remsetFilterReceipt.noOrigin.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastNoOriginSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kBadTarget:
            g_remsetFilterReceipt.badTarget.fetch_add(1, std::memory_order_relaxed);
            g_remsetFilterReceipt.lastBadTargetSlot.store(slot, std::memory_order_relaxed);
            break;
        case RemsetFilterReceiptReason::kNone:
            break;
    }
}
#endif
}
