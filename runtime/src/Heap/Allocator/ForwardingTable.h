// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"
#include "Heap/Collector/ZForwarding.h"

namespace MapleRuntime {
class RegionInfo;
class BaseObject;

// zForwardingTable.hpp:32-52 — granule map of ZForwarding*.
// Two maps: membership (get/insert/remove) unlinks at Dispel; entries live until
// ClearEntries. ZGC has one map because reset_relocation_set is the only unlink.
class ForwardingTable {
public:
    // Compile-time: FindToVersion prefers a stored entry, then falls back to geometry
    // until the entry exists.  After route retirement only the entry can answer.
    static constexpr bool kConsumeEntries = true;
    // Step ③: an armed region (GetEntries != null) answers only from the table.
    // Miss is "no to" — never invent a destination from route geometry.
    // Unarmed regions still use geometry (transition). zForwarding.inline.hpp:248-252.
    static constexpr bool kEntriesSoleWhenArmed = true;
    // PORT_ZFORWARDING step ②: IsFromObject / membership consume the table.
    // Default false — product still trusts the region-type path. Flip only when
    // NoteCompare agree rate is 100% (or every disagree is a legacy defect).
    static constexpr bool kZfwdTableConsume = false;

    enum class ToAnswer : uint8_t { ArmedHit, ArmedMiss, Unarmed };

    static void Initialize(MAddress heapStart, size_t heapSize, size_t unitSize);

    // Dual-write hook from RegionInfo (PrepareForwardable / SetRegionType FROM*).
    static void Insert(MAddress regionStart, size_t regionSize, RegionInfo* region);
    static void Remove(MAddress regionStart, size_t regionSize);
    static void EnsureEntries(RegionInfo* region);
    static void ClearEntries(MAddress regionStart, size_t regionSize);
    // After-copy Exempt parks a live table; ClearEntries unlinks it into the
    // retired generation. FindTo/LookupTo still scan that generation, so a
    // kept page re-armed next cycle would find() last cycle's dest. Drop the
    // covering tables at the next install (zRelocationSet.cpp:91-96).
    static void DropRetiredCovering(MAddress regionStart, size_t regionSize);
    static void Retire(ZForwarding* tab);
    static void ReclaimRetired(const char* why);

    // zForwardingTable.inline.hpp:43-62
    static ZForwarding* get(MAddress addr);
    static void insert(ZForwarding* forwarding);
    static void remove(ZForwarding* forwarding);

    static ZForwarding* Get(MAddress addr) { return get(addr); }
    static ZForwarding* GetEntries(MAddress addr);

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
    static uint32_t EstimateLiveObjects(RegionInfo* region, size_t regionSize);
};
} // namespace MapleRuntime

#endif // MRT_FORWARDING_TABLE_H
