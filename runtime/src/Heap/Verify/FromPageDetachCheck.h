// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FROM_PAGE_DETACH_CHECK_H
#define MRT_FROM_PAGE_DETACH_CHECK_H

#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// FROM_PAGE_DETACH_GATE central checkpoint. Every from-page free/reuse funnel
// calls the same evidence predicate and refuses reuse until relocation has
// detached the page-local state.
//
// ZGC keeps page memory and forwarding metadata on separate clocks:
// zRelocate.cpp:1041-1047 frees the page when that page's relocation completes,
// zForwarding.cpp:86-116 retains the independent forwarding object, and
// zRelocationSet.cpp:191-197 destroys forwardings only at relocation-set reset.
namespace FromPageDetach {

enum class Site : uint8_t {
    COLLECT_FROM_GARBAGE = 0,
    TAKE_GARBAGE = 1,
    TAKE_AFTER_DISPEL = 2,
    RECLAIM_DIRTY = 3,
    RECLAIM_MARK_QUARANTINE = 4,
    RELEASE_REGION = 5,
    RELEASE_GARBAGE_UNITS = 6,
    TAKE_GARBAGE_REUSE = 7,
    TAKE_DIRTY_REUSE = 8,
    TAKE_RELEASED_REUSE = 9,
    CLEAR_UNITS = 10,
    RELEASE_UNITS = 11,
    INIT_FREE_UNITS = 12,
    INIT_REGION_INFO = 13,
    MAJOR_RECHECK = 14,
    SITE_COUNT = 15
};

struct Counters {
    uint64_t checks;
    uint64_t withEvidence;
    uint64_t blocked;
    uint64_t activeTable;
    uint64_t retiredTable;
    uint64_t routeDestHeld;
    uint64_t forwardingPositive;
    uint64_t forwardingReaders;
    uint64_t forwardingClaimed;
    uint64_t forwardingReleased;
    uint64_t copyInflight;
};

struct QuarantineCounters {
    uint64_t admitted;
    uint64_t released;
    uint64_t recheckHeld;
    uint64_t peakEntries;
};

enum class Action : uint8_t {
    OBSERVE = 0,
    // The major close rechecks page-local detach evidence. Retired forwarding
    // metadata is independently owned and is not a page-reuse condition.
    MAJOR_CLOSE = 1,
};

// Page-local relocation retention is always enforced (zRelocate.cpp:1041-1047).
bool GateEnabled();

// A caller may carry a successful precheck across a compound reuse operation
// (for example InitFreeUnits converting every subordinate unit). The scope
// suppresses blocking only on that thread; observations are still counted.
class ReusePermitScope {
public:
    ReusePermitScope();
    ~ReusePermitScope();
    ReusePermitScope(const ReusePermitScope&) = delete;
    ReusePermitScope& operator=(const ReusePermitScope&) = delete;
};

// The sole page evidence predicate. A range with an active page reader,
// in-flight copy, or attached route owner is not reusable. Active and retired
// forwarding objects are counted here, but their independent lifetime does not
// hold page memory (zForwarding.cpp:86-116; zRelocationSet.cpp:191-197).
bool FromPageDetachCheck(const RegionInfo* region, Site site, Action action = Action::OBSERVE);

Counters GetCounters(Site site);
QuarantineCounters GetQuarantineCounters();
void NoteQuarantineAdmitted(uint64_t entriesNow);
void NoteQuarantineReleased();
void NoteQuarantineRecheckHeld();
const char* SiteName(Site site);
void DumpSummary();

} // namespace FromPageDetach
} // namespace MapleRuntime

#endif // MRT_FROM_PAGE_DETACH_CHECK_H
