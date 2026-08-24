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
    // A retained, fully-installed forwarding carried across object copy and
    // receipt publication. ClearEntries seals the table and waits for every
    // Publication to drain before unlinking it (zRelocate.cpp:354-379,
    // zGeneration.cpp:253-265,276-284).
    class Publication {
    public:
        Publication() : forwarding(nullptr) {}
        ~Publication();
        Publication(Publication&& other) noexcept;
        Publication& operator=(Publication&& other) noexcept;
        explicit operator bool() const { return forwarding != nullptr; }

        Publication(const Publication&) = delete;
        Publication& operator=(const Publication&) = delete;

    private:
        explicit Publication(ZForwarding* forwarding) : forwarding(forwarding) {}
        void Release();

        ZForwarding* forwarding;
        friend class ForwardingTable;
    };

    // Compile-time: FindToVersion prefers a stored entry, then falls back to geometry
    // until the entry exists.  After route retirement only the entry can answer.
    static constexpr bool kConsumeEntries = true;
    // Step ③: an armed region (GetEntries != null) answers only from the table.
    // Miss is "no to" — never invent a destination from route geometry.
    // Unarmed regions still use geometry (transition). zForwarding.inline.hpp:248-252.
    static constexpr bool kEntriesSoleWhenArmed = true;
    // PORT_ZFORWARDING step ②: IsFromObject / membership consume the table.
    // Disagreements vs region-type are legacyOnly at FROM/UNMOVABLE_FROM after
    // Dispel unlinked membership — old path reading a type that has moved.
    // tableOnly is the other old-path miss (type not yet FROM while table is).
    static constexpr bool kZfwdTableConsume = true;

    enum class ToAnswer : uint8_t { ArmedHit, ArmedMiss, Unarmed };

    static bool Initialize(MAddress heapStart, size_t heapSize, size_t unitSize);

    // Explicit cycle boundary. This is the only operation allowed to reopen a
    // region span after ClearEntries sealed its previous generation.
    static bool PreparePublicationGeneration(MAddress regionStart, size_t regionSize);
    // Full table installation at PrepareForwardableRegion, before any copy in
    // this generation can begin.
    static bool InstallPublicationBeforeCopy(MAddress regionStart, size_t regionSize, RegionInfo* region);
    // Publish FROM membership immediately, but defer the live-sized attached array
    // until PrepareForwardableRegion has a closed mark face.
    static bool InsertProvisional(MAddress regionStart, size_t regionSize, RegionInfo* region);
    static void Remove(MAddress regionStart, size_t regionSize);
    static void ClearEntries(MAddress regionStart, size_t regionSize);
    // After-copy Exempt parks a live table; ClearEntries unlinks it into the
    // retired generation. FindTo/LookupTo still scan that generation, so a
    // kept page re-armed next cycle would find() last cycle's dest. Drop the
    // covering tables at the next install (zRelocationSet.cpp:91-96).
    static void DropRetiredCovering(MAddress regionStart, size_t regionSize);
    static void Retire(ZForwarding* tab);
    static void ReclaimRetired(const char* why);
    // Measurement face for FROM_PAGE_DETACH_GATE. True while either retired
    // generation still contains a forwarding whose from range overlaps this
    // region. It never changes table lifetime.
    static bool RetiredCovers(MAddress regionStart, size_t regionSize);

    // zForwardingTable.inline.hpp:43-62
    static ZForwarding* get(MAddress addr);
    static void insert(ZForwarding* forwarding);
    static void remove(ZForwarding* forwarding);

    static ZForwarding* Get(MAddress addr) { return get(addr); }
    static ZForwarding* GetEntries(MAddress addr);

    // Copy producer: may allocate/install the explicitly prepared generation,
    // then retains it across copy and receipt publication.
    static Publication EnsurePublicationBeforeCopy(RegionInfo* region, MAddress from);
    // After-copy consumer: retain the current generation only while it remains
    // open. It never allocates, installs, replaces, or reopens a table.
    static Publication RetainOpenPublicationAfterCopy(RegionInfo* region, MAddress from);
    static ZForwarding::Receipt InstallMapping(const Publication& publication, MAddress from, MAddress to);
    static MAddress InsertMapping(const Publication& publication, MAddress from, MAddress to);
    // Out of line so the unit runner exercises the product SO's publication
    // decision instead of compiling a private test copy.
    static bool ReceiptAllowsForwarded(MAddress mapped);
    static uint64_t StaleToLifeCount();
    static MAddress FindTo(MAddress from);
    // Retired-only lookup for a bad-colour load whose from page has already
    // lost ghost/membership. The retired table is self-contained; requiring
    // the ghost first would make this answer unreachable.
    static MAddress FindRetiredTo(MAddress from);
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
    static ZForwarding* EnsureEntriesLocked(RegionInfo* region);
};
} // namespace MapleRuntime

#endif // MRT_FORWARDING_TABLE_H
