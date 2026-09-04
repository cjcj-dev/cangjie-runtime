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
enum class Generation : uint8_t;
class RegionInfo;
class BaseObject;
struct LiveInfo;

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

    // Transport detail used by WCollector's public FindToVersionResult.
    // ArmedMiss means a retained/queryable carrier was searched. Unavailable
    // means a carrier or closed publication exists but can no longer be
    // queried. Unarmed remains the pre-publication route-geometry state.
    enum class ToAnswer : uint8_t { ArmedHit, ArmedMiss, Unavailable, Unarmed };
    enum class ToUnavailableCause : uint8_t {
        None = 0,
        ActiveRetainRejected = 1,
        RetiredUnavailable = 2,
        PublicationClosed = 4,
        TableDestroyed = 8,
        NeverInstalled = 16,
    };

    // Decision record from one LookupTo invocation.  These are the exact local
    // values consumed by its final classification; no caller re-queries the
    // active/retired/publication carriers to construct diagnostics.
    struct LookupResult {
        MAddress to;
        ToAnswer answer;
        ToUnavailableCause unavailableCause;
        bool activeCandidate;
        bool activeRetained;
        ToAnswer activeAnswer;
        ToAnswer retiredAnswer;
        bool publicationClosed;
        bool currentMembership;
        uintptr_t tableId;
    };

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
    // The relocation-set reset edge. ZRelocationSet::reset destroys every ZForwarding the set
    // owned (zRelocationSet.cpp:191-197) and ZForwardingTable::get answers only for a page still
    // in the live set (zForwardingTable.inline.hpp:36-46), so once a relocation ends nothing of
    // it can be handed to a later reader. Removing membership alone leaves the publication open,
    // and an open publication lets a later region-type change re-publish the finished cycle's
    // carrier as current membership. Seal and unlink in one operation.
    static void RetireMembershipAtDispel(MAddress regionStart, size_t regionSize);
    static void ClearEntries(MAddress regionStart, size_t regionSize);
    static void Retire(ZForwarding* tab);
    // The sole retired-table destruction edge. The caller is the old
    // remap-young-roots coverage closure, after ClearEntries has already
    // drained every retained publication owner (zGeneration.cpp:1458-1523;
    // zForwarding.cpp:171-181).
    static void ReclaimRetired(const char* why);
    // Next same-generation mark-end coverage receipt. A8 may call ReclaimRetired
    // but must not manufacture this epoch (zGeneration.cpp:276-285).
    static void PublishMarkCoverage(Generation gen);
    static uint64_t MarkCoverageEpoch(Generation gen);
    static bool RetiredDestroyEligible(ZForwarding* tab);
    static Publication RetainCovering(MAddress from);
    static size_t RetiredQueueSize();
    // Measurement face for FROM_PAGE_DETACH_GATE. True while either retired
    // generation still contains a forwarding whose from range overlaps this
    // region. It never changes table lifetime.
    static bool RetiredCovers(MAddress regionStart, size_t regionSize);
    // Product hard condition for released-cache enqueue (zForwarding.cpp:171-181
    // detach_page waits ref_count==0). Not gated by diagnostic switches.
    static bool HasLiveCarrier(MAddress regionStart, size_t regionSize);

    // zForwardingTable.inline.hpp:43-62
    static ZForwarding* get(MAddress addr);
    static void insert(ZForwarding* forwarding);
    static void remove(ZForwarding* forwarding);

    static ZForwarding* Get(MAddress addr) { return get(addr); }
    static ZForwarding* GetEntries(MAddress addr);
    // Active entries, then membership, then a retired generation that still
    // covers `addr`. ClassifyCompactedMiss must see the same carrier LookupTo
    // uses after ClearEntries (zForwardingTable.inline.hpp:36-46).
    static ZForwarding* GetCovering(MAddress addr);
    // Product connection points for the dual carrier. Publication copies the
    // from-page view into the already-installed ZForwarding; every consumer
    // resolves the view back through the table rather than RegionInfo storage.
    static bool PublishFromPageView(RegionInfo* region, LiveInfo* liveInfo, uint64_t epoch,
                                    MAddress topAtStart, MAddress markStartAllocPtr,
                                    uint64_t liveByteCount, uint8_t owner,
                                    uint8_t largeMarked, RegionLifeId lifeId);
    static const ZForwarding::FromPageView* GetFromPageView(RegionInfo* region);

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
    // Kept producer acknowledgement: the observed FORWARDED header still
    // consumes this retired receipt after active generation takeover.
    static MAddress RequireRetiredTo(MAddress from);
    static bool EntriesArmed(MAddress from);
    static LookupResult LookupTo(MAddress from);
    static uint64_t ArmedHitCount();
    static uint64_t ArmedMissCount();
    static uint64_t UnavailableCount();
    static uint64_t UnarmedCount();

#if defined(MRT_TESTABLE_INTERNALS)
    using LookupRetainHook = void (*)(void*);
    static void SetLookupRetainHook(LookupRetainHook hook, void* context);
    // Deterministic rendezvous immediately before a fresh receipt enters the
    // destination-life registration critical section.
    using ReceiptLifeRegisterHook = void (*)(void*);
    static void SetReceiptLifeRegisterHook(ReceiptLifeRegisterHook hook, void* context);
#endif

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
