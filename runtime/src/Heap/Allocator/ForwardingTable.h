// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"
#include "Heap/Allocator/ForwardingEntry.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// PORT_ZFORWARDING ②b/③: granule map (step 1) plus per-page _entries (this step).
// Destination is whatever insert stored.  Miss is null.  No geometry.
class ForwardingTable {
public:
    // Compile-time: FindToVersion prefers a stored entry, then falls back to geometry
    // until the entry exists.  After route retirement only the entry can answer.
    static constexpr bool kConsumeEntries = true;
    // Step ③: an armed region (GetEntries != null) answers only from the table.
    // Miss is "no to" — never invent a destination from route geometry.
    // Unarmed regions still use geometry (transition). zForwarding.inline.hpp:248-252.
    static constexpr bool kEntriesSoleWhenArmed = true;

    enum class ToAnswer : uint8_t { ArmedHit, ArmedMiss, Unarmed };

    static void Initialize(MAddress heapStart, size_t heapSize, size_t unitSize);

    static void Insert(MAddress regionStart, size_t regionSize, RegionInfo* region);
    static void Remove(MAddress regionStart, size_t regionSize);
    static void EnsureEntries(RegionInfo* region);
    static void ClearEntries(MAddress regionStart, size_t regionSize);
    // Unlinked-but-not-freed tables, and the point at which they actually go away. ZGC keeps the
    // same gap by construction: entries live in an arena recycled a whole phase after the page dies.
    static void Retire(ForwardingEntries* tab);
    // CUT-2: free only tables retired before the previous cycle (f87a lag + 1).
    // Stragglers that miss this cycle's map still resolve via FindTo on the
    // retired generation (Barrier.cpp:718 staleguard).
    static void ReclaimRetired(const char* why);
    static uint64_t RetiredLiveBytes();
    static uint64_t RetiredLivePeakBytes();

    static RegionInfo* Get(MAddress addr);
    static ForwardingEntries* GetEntries(MAddress addr);
    // After ClearEntries unlinks the map, still answer from the retired generation.

    // After copy: zRelocate.cpp:367-372
    static MAddress InsertMapping(MAddress from, MAddress to);
    static MAddress FindTo(MAddress from);
    static bool EntriesArmed(MAddress from);
    static MAddress LookupTo(MAddress from, ToAnswer* answer = nullptr);
    static uint64_t ArmedHitCount();
    static uint64_t ArmedMissCount();
    static uint64_t UnarmedCount();

    static void NoteCompare(MAddress addr, bool legacy);
    static void NoteDestCompare(MAddress from, MAddress geometricTo);
    static void DumpCompare(const char* why);

    static bool Ready();

private:
    static size_t IndexFor(MAddress addr);
};
} // namespace MapleRuntime

#endif // MRT_FORWARDING_TABLE_H
