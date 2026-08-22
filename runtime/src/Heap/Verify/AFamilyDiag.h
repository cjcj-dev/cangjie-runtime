// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// afamily: who painted a still-reachable holder live while its child was reclaimed
// (VALSIDE family=A-zeroed typeInfo=0 hasTo=0). Product paths unchanged. Gate:
//   MRT_GCV2_AFAMILY=1
// Default off.

#ifndef MRT_AFAMILY_DIAG_H
#define MRT_AFAMILY_DIAG_H

#include <cstdint>

namespace MapleRuntime {

class BaseObject;
class RegionInfo;
class Collector;

namespace AFamilyDiag {

enum Channel : uint8_t {
    CH_NONE = 0,
    CH_TRACE = 1,         // ConcurrentMarkingWork !wasMarked / young claim
    CH_ALLOC_WATER = 2,   // AllocatedAfterMarkStart (RegionInfo.h:491/501)
    CH_TRACE_REGION = 3,  // isTraceRegion implicit-black (HandleTraceRegions)
    CH_COMPACT = 4,       // CompactRegion copy without field push
};

bool Enabled();

void NoteClaim(BaseObject* obj, Channel ch);
void OnAZeroed(BaseObject* victim, BaseObject* holder, const void* slot, uint8_t phase, int hasTo);
void ReportSummary(const char* point);

} // namespace AFamilyDiag
} // namespace MapleRuntime

#endif
