// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <atomic>
#include <cstddef>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class RegionInfo;

// Step 1 of ops/design/PORT_ZFORWARDING.md: the address-keyed relocation-set table ZGC answers
// "is this address being relocated" with, built alongside the existing region/route machinery so
// the two can be compared before either is trusted.
//
// Shape is ZGranuleMap<ZForwarding*> (zGranuleMap.hpp): a flat array indexed by granule, with the
// granule being our allocation unit.  ZForwardingTable::get(addr) is one index and one load -- it
// asks the address, never a region's type.  That distinction is the point of the port: a region
// type is a property that moves as relocation progresses (FROM -> LONE_FROM -> GARBAGE -> reused),
// and several defects this session came from predicates reading it at the wrong moment.
//
// This step deliberately changes no behaviour.  It records, and it counts disagreements with the
// existing answer.  Every disagreement is either a bug in the table (fix it here) or a bug in the
// current implementation (that is the finding).  Turning consumers over to it is step 2.
class ForwardingTable {
public:
    // Sized from the heap's unit count; safe to call more than once.
    static void Initialize(MAddress heapStart, size_t heapSize, size_t unitSize);

    // A region enters the relocation set (our PrepareForwardableRegion) / leaves it (DispelGhost).
    static void Insert(MAddress regionStart, size_t regionSize, RegionInfo* region);
    static void Remove(MAddress regionStart, size_t regionSize);

    // ZForwardingTable::get -- nullptr means "not being relocated".
    static RegionInfo* Get(MAddress addr);

    // Consistency against the existing answer, counted rather than assumed.
    // `legacy` is what IsFromObject/IsGhostFromObject say today.
    static void NoteCompare(MAddress addr, bool legacy);
    static void DumpCompare(const char* why);

    static bool Ready();

private:
    static size_t IndexFor(MAddress addr);
};
} // namespace MapleRuntime

#endif // MRT_FORWARDING_TABLE_H
