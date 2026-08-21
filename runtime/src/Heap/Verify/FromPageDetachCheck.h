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

// Phase 1 of FROM_PAGE_DETACH_GATE: a measurement-only central checkpoint.
// Every from-page free/reuse funnel calls the same function. It records evidence
// that the old page still has consumers, but never waits, rejects or changes a
// region/list/tree. Phase 2 may change the decision made after this observation;
// it must not grow a second evidence predicate.
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
    SITE_COUNT = 14
};

struct Counters {
    uint64_t checks;
    uint64_t withEvidence;
    uint64_t activeTable;
    uint64_t retiredTable;
    uint64_t routeDestHeld;
    uint64_t forwardingPositive;
    uint64_t forwardingReaders;
    uint64_t forwardingClaimed;
    uint64_t forwardingReleased;
    uint64_t copyInflight;
};

// The sole evidence predicate. Phase 1 always returns true so callers cannot
// accidentally turn the measuring arm into a product gate.
bool FromPageDetachCheck(const RegionInfo* region, Site site);

Counters GetCounters(Site site);
const char* SiteName(Site site);
void DumpSummary();

} // namespace FromPageDetach
} // namespace MapleRuntime

#endif // MRT_FROM_PAGE_DETACH_CHECK_H
