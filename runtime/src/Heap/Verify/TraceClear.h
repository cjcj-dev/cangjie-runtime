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

// Ring of recent payload-zero ranges (CompactRegion tail / ClearUnits).
// Gate (default off): MRT_GCV2_TRACE_CLEAR=1
// Lookup at invalid-minor-root: was obj inside a range that was zeroed?
class TraceClear {
public:
    static bool Enabled();

    // kind: "compact" | "clear_units"
    static void NoteRange(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore);

    // F3-only lifecycle event, recorded in the same ring as payload clears.
    static void NoteRegionEvent(MAddress start, size_t size, const char* kind, void* region, size_t liveBefore,
                                unsigned int isGhost, unsigned int regionType, unsigned int routeState);

    // Returns true if addr is in any recorded range; fills detail line into buf.
    static bool Lookup(MAddress addr, char* buf, size_t bufSize);

    // Returns the newest matching event from the requested GC cycle.
    static bool LookupKind(MAddress addr, const char* kind, uint64_t gcStartNs, char* buf, size_t bufSize);

    // Optional: dump last N ranges (VLOG).
    static void DumpRecent(size_t n);

    // Blocking test (default off): MRT_GCV2_SKIP_COMPACT_MEMSET=1
    // Skip CompactRegion tail memset so zero-header path is blocked.
    static bool SkipCompactMemset();
};

} // namespace MapleRuntime

#endif // MRT_HEAP_TRACE_CLEAR_H
