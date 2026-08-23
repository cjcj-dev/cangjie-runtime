// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Verify/M0ExitDiagnostics.h"

#include <atomic>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/ZForwarding.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace ZgcInvariants {
uint64_t WCollectorFlipSeqForProbe();
}

namespace M0ExitDiagnostics {
namespace {

struct StackMapContext {
    bool active = false;
    bool valid = false;
    StackMapInvalidReason reason = StackMapInvalidReason::NONE;
    uintptr_t startIP = 0;
    uintptr_t frameIP = 0;
    uintptr_t frameFA = 0;
};

thread_local StackMapContext g_stackMap;
std::atomic<uint64_t> g_total{ 0 };
std::atomic<uint64_t> g_s0{ 0 };
std::atomic<uint64_t> g_s1{ 0 };
std::atomic<uint64_t> g_rootFix{ 0 };
std::atomic<uint64_t> g_readBarrier{ 0 };

const char* ExitName(Exit exit)
{
    return exit == Exit::RootFix ? "root-fix" : "read-barrier";
}

const char* StackMapReasonName(StackMapInvalidReason reason)
{
    switch (reason) {
        case StackMapInvalidReason::NONE:
            return "none";
        case StackMapInvalidReason::ZERO_ENTRIES:
            return "present-but-zero-entries";
        case StackMapInvalidReason::PC_MISS:
            return "pc-miss-exact";
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            return "zero-root-indices";
    }
    return "unknown";
}

} // namespace

StackMapScope::StackMapScope(bool valid, StackMapInvalidReason reason, uintptr_t startIP, uintptr_t frameIP,
                             uintptr_t frameFA)
    : hadPrevious(g_stackMap.active),
      previousValid(g_stackMap.valid),
      previousReason(g_stackMap.reason),
      previousStartIP(g_stackMap.startIP),
      previousFrameIP(g_stackMap.frameIP),
      previousFrameFA(g_stackMap.frameFA)
{
    g_stackMap = StackMapContext{ true, valid, reason, startIP, frameIP, frameFA };
}

StackMapScope::~StackMapScope()
{
    g_stackMap = StackMapContext{ hadPrevious, previousValid, previousReason, previousStartIP, previousFrameIP,
                                  previousFrameFA };
}

void Note(Exit exit, BaseObject* target, const void* slot, BaseObject* holder, uint8_t phase)
{
    const uint64_t n = g_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (exit == Exit::RootFix) {
        g_rootFix.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_readBarrier.fetch_add(1, std::memory_order_relaxed);
    }

    RegionInfo* region = (target != nullptr && Heap::IsHeapAddress(target))
        ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target))
        : nullptr;
    const MAddress from = reinterpret_cast<MAddress>(target);
    ZForwarding* active = region == nullptr ? nullptr : ForwardingTable::GetEntries(from);
    const MAddress activeTo = (active != nullptr && active->covers(from)) ? active->find(from) : 0;
    const MAddress retiredTo = region == nullptr ? 0 : ForwardingTable::FindRetiredTo(from);

    // FORWARDED is published only after CopyObject completed. Keep it as an independent witness:
    // asking only the lookup that just missed would make S1 unobservable by construction.
    const bool copyPublished = target != nullptr && region != nullptr && target->IsForwarded();
    const bool hasTo = activeTo != 0 || retiredTo != 0 || copyPublished;
    (hasTo ? g_s1 : g_s0).fetch_add(1, std::memory_order_relaxed);

    unsigned youngMark = 2; // 2 = not a young region, so no young mark face exists
    unsigned oldMark = 0;
    uint64_t youngEpoch = 0;
    uint64_t oldEpoch = 0;
    if (region != nullptr && target != nullptr) {
        if (region->IsYoungRegion()) {
            youngMark = region->IsMarkedObject(region->GetMarkView<Generation::Young>(), target) ? 1u : 0u;
        }
        oldMark = region->IsMarkedObject(region->GetMarkView<Generation::Old>(), target) ? 1u : 0u;
        youngEpoch = region->GetMarkSnapshotEpoch<Generation::Young>();
        oldEpoch = region->GetMarkSnapshotEpoch<Generation::Old>();
    }

    const char* stackMap = !g_stackMap.active ? "not-frame" : (g_stackMap.valid ? "valid" : "invalid");
    const uint64_t regionLife = region == nullptr ? 0 : region->GetRegionLifeId();
    const uint64_t activeLife = active == nullptr ? 0 : active->page_life_id();
    const unsigned activeCurrent = active == nullptr ? 0u
        : (active->page_life_current(RegionLifeClock::Carrier::ARMED_ENTRY) ? 1u : 0u);
    const unsigned regionType = region == nullptr ? 0xffu : static_cast<unsigned>(region->GetRegionType());
    const unsigned routeState = region == nullptr ? 0xffu : static_cast<unsigned>(region->GetRouteState());

    // M0 is rare and every occurrence matters. Do not power-of-two throttle: throttling would
    // recreate the silent bucket this record exists to remove.
    LOG(RTLOG_ERROR,
        "[M0][classify] n=%llu class=%s hasTo=%u exit=%s target=%p slot=%p holder=%p phase=%u "
        "copyPublished=%u youngMark=%u oldMark=%u stackMap=%s stackReason=%s startIP=%p frameIP=%p "
        "frameFA=%p active=%u activeTo=%p retiredTo=%p flipEpoch=%llu regionLife=%llu activeLife=%llu "
        "activeCurrent=%u youngEpoch=%llu oldEpoch=%llu regionType=%u routeState=%u",
        static_cast<unsigned long long>(n), hasTo ? "S1" : "S0", hasTo ? 1u : 0u, ExitName(exit),
        static_cast<void*>(target), slot, static_cast<void*>(holder), static_cast<unsigned>(phase),
        copyPublished ? 1u : 0u, youngMark, oldMark, stackMap, StackMapReasonName(g_stackMap.reason),
        reinterpret_cast<void*>(g_stackMap.startIP), reinterpret_cast<void*>(g_stackMap.frameIP),
        reinterpret_cast<void*>(g_stackMap.frameFA), active == nullptr ? 0u : 1u,
        reinterpret_cast<void*>(activeTo), reinterpret_cast<void*>(retiredTo),
        static_cast<unsigned long long>(ZgcInvariants::WCollectorFlipSeqForProbe()),
        static_cast<unsigned long long>(regionLife), static_cast<unsigned long long>(activeLife), activeCurrent,
        static_cast<unsigned long long>(youngEpoch), static_cast<unsigned long long>(oldEpoch), regionType,
        routeState);
}

#if defined(MRT_GC_UNIT_TEST_ACCESS)
Counts GetCounts()
{
    return Counts{ g_total.load(std::memory_order_relaxed), g_s0.load(std::memory_order_relaxed),
                   g_s1.load(std::memory_order_relaxed), g_rootFix.load(std::memory_order_relaxed),
                   g_readBarrier.load(std::memory_order_relaxed) };
}
#endif

} // namespace M0ExitDiagnostics
} // namespace MapleRuntime
