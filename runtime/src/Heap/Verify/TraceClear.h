// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_TRACE_CLEAR_H
#define MRT_HEAP_TRACE_CLEAR_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// Ring of recent payload-zero ranges (CompactRegion tail / ClearUnits) and
// region lifecycle events (CollectRegion / garbage_reuse / dirty_take / ...).
// Gate (default off): MRT_GCV2_TRACE_CLEAR=1 | MRT_GCV2_F3_REGION=1 | MRT_GCV2_F3_DEATH=1
// Lookup at invalid-minor-root: was obj inside a range that was zeroed?
class TraceClear {
public:
    static bool Enabled();
    static bool DeathEnabled();

    // kind: "compact" | "clear_units" | ...
    static void NoteRange(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore);

    // Lifecycle event, recorded in the same ring as payload clears.
    // gcKind: 0=unknown 1=minor 2=major (only meaningful when DeathEnabled).
    static void NoteRegionEvent(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                                unsigned int isGhost, unsigned int regionType, unsigned int routeState,
                                unsigned int gcKind = 0, size_t gcIndex = 0);

    // Returns true if addr is in any recorded range; fills detail line into buf.
    static bool Lookup(MAddress addr, char* buf, size_t bufSize);

    // Returns the newest matching event from the requested GC cycle.
    static bool LookupKind(MAddress addr, const char* kind, uint64_t gcStartNs, char* buf, size_t bufSize);

    // Dump every ring entry covering addr (newest first) into VLOG; also fill a
    // one-line summary of the first kill event (collect_region / clear_units /
    // garbage_reuse / dirty_take) into killBuf. Returns number of matching events.
    static size_t DumpHistory(MAddress addr, char* killBuf, size_t killBufSize);

    // Ring capacity / total / wrap count for overflow reporting.
    static void RingStats(size_t& capacity, size_t& total, size_t& wrapCount);

    // Optional: dump last N ranges (VLOG).
    static void DumpRecent(size_t n);

    // Blocking test (default off): MRT_GCV2_SKIP_COMPACT_MEMSET=1
    // Skip CompactRegion tail memset so zero-header path is blocked.
    static bool SkipCompactMemset();
};

} // namespace MapleRuntime

#endif // MRT_HEAP_TRACE_CLEAR_H
