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
// calls the same evidence predicate. The product-default OFF arm only measures;
// CJRT_FROM_REUSE_GATE=1 refuses reuse and lets the caller quarantine the range.
//
// ZGC anchors: zForwarding.cpp:171-181 waits for ref_count==0 in detach_page;
// zForwarding.inline.hpp makes retain/release the admission protocol; and
// zRelocate.cpp:1018-1047 releases the page only after the remap closure.
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
    uint64_t txnEvidence;
    uint64_t txnHandles;
    uint64_t txnOutstanding;
};

struct QuarantineCounters {
    uint64_t admitted;
    uint64_t released;
    uint64_t recheckHeld;
    uint64_t peakEntries;
};

enum class Action : uint8_t {
    OBSERVE = 0,
    // A major remap closure is the grace point that may consume a retired
    // answer, but only after every other evidence leg is absent.
    MAJOR_CLOSE = 1,
};

// Product default is OFF. Only the exact value "1" enables phase-2 blocking.
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

// The sole evidence predicate. With the product-default OFF it is a measuring
// arm and always returns true; CJRT_FROM_REUSE_GATE=1 refuses evidence.
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
