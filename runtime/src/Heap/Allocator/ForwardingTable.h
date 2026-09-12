// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
#include "Heap/Collector/ZForwarding.h"

namespace MapleRuntime {
enum class Generation : uint8_t;
class RegionInfo;
class RegionList;
class BaseObject;
struct LiveInfo;

// zForwardingTable.hpp:32-52 — granule map of ZForwarding*.
// Map entries borrow the objects owned by the generation relocation sets.
class ForwardingTable {
public:
    // Borrowed forwarding identity. The generation's relocation set owns it
    // through mark/remap and reset (zRelocationSet.cpp:172-200).
    // Access to source bytes separately requires retain_page().
    class Owner {
    public:
        Owner() = default;
        explicit operator bool() const { return forwarding != nullptr; }
        ZForwarding* get() const { return forwarding; }
        ZForwarding* operator->() const { return forwarding; }
    private:
        explicit Owner(ZForwarding* value) : forwarding(value) {}
        ZForwarding* forwarding{ nullptr };
        friend class ForwardingTable;
    };

    static Owner RetainPageOwner(const RegionInfo* region);
    static void ClearPageOwner(RegionInfo* region);

    // Copy and receipt publication borrow the installed forwarding. The page
    // retain is held by the relocation operation (zRelocate.cpp:354-420).
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
    static constexpr bool kZfwdTableConsume = true;

    // Transport detail used by WCollector's public FindToVersionResult.
    // ArmedMiss means an installed forwarding was searched. Unarmed means
    // the address has no forwarding in the currently installed sets.
    enum class ToAnswer : uint8_t { ArmedHit, ArmedMiss, Unarmed };
    static constexpr size_t kNeverInstalledCarrierLimit = 16;
    static constexpr size_t kNeverInstalledReverseLimit = 8;
    struct CarrierIdentity {
        uintptr_t tableId{ 0 };
        MAddress start{ 0 };
        size_t size{ 0 };
        uint8_t tableGeneration{ 0 };
        uint64_t fromPageEpoch{ 0 };
        RegionLifeId fromPageLifeId{ 0 };
        ToAnswer answer{ ToAnswer::Unarmed };
    };
    struct ReverseReceiptIdentity {
        uintptr_t tableId{ 0 };
        MAddress from{ 0 };
    };
    struct NeverInstalledSnapshot {
        CarrierIdentity carriers[kNeverInstalledCarrierLimit]{};
        ReverseReceiptIdentity reverseReceipts[kNeverInstalledReverseLimit]{};
        size_t carrierCount{ 0 };
        size_t carrierTotal{ 0 };
        size_t reverseCount{ 0 };
        size_t reverseTotal{ 0 };
        bool carrierOverflow{ false };
        bool reverseOverflow{ false };
        bool scanOverflow{ false };
    };

    // Decision record from one LookupTo invocation.  These are the exact local
    // values consumed by its final classification; no caller re-queries the
    // forwarding map to construct diagnostics.
    struct LookupResult {
        MAddress to{0};
        ToAnswer answer{ToAnswer::Unarmed};
        bool activeCandidate{false};
        ToAnswer activeAnswer{ToAnswer::Unarmed};
        bool currentMembership{false};
        // Identity of the table searched, including an armed miss.
        // All carrier fields below belong to that same table.
        uintptr_t tableId{0};
        MAddress carrierStart{ 0 };
        uint64_t fromPageEpoch{ 0 };
        RegionLifeId fromPageLifeId{ 0 };
        bool forwardingSnapshotValid{ false };
    };

    static bool Initialize(MAddress heapStart, size_t heapSize, size_t unitSize);

    // Budget the closed installation pass before publishing its first table.
    // The generation set owns every allocation through reset.
    static bool BeginForwardingArena(Generation gen, RegionList& regions);

    static bool InstallPublicationBeforeCopy(MAddress regionStart, size_t regionSize, RegionInfo* region);
    // zGeneration.cpp:276-284: unlink every member, then destroy the set.
    static void ResetRelocationSet(Generation gen);
#if defined(MRT_TESTABLE_INTERNALS)
    static const ForwardingAllocator* ArenaForTest(Generation gen);
#endif
    // zForwardingTable.inline.hpp:43-62
    static ZForwarding* get(MAddress addr);
    static void insert(ZForwarding* forwarding);
    static void remove(ZForwarding* forwarding);

    static ZForwarding* Get(MAddress addr) { return get(addr); }
    static ZForwarding* GetEntries(MAddress addr);
    // All queries use the same map (zForwardingTable.inline.hpp:36-46).
    static ZForwarding* GetCovering(MAddress addr);
    static void VisitAll(const std::function<void(ZForwarding*)>& visitor);
    // Product connection points for the dual carrier. Publication copies the
    // from-page view into the already-installed ZForwarding; every consumer
    // resolves the view back through the table rather than RegionInfo storage.
    static bool PublishFromPageView(RegionInfo* region, LiveInfo* liveInfo, uint64_t epoch,
                                    MAddress topAtStart, MAddress markStartAllocPtr,
                                    uint64_t liveByteCount, uint8_t owner,
                                    uint8_t largeMarked, RegionLifeId lifeId);
    static const ZForwarding::FromPageView* GetFromPageView(RegionInfo* region);

    // Copy producer borrows the set installed before relocation starts.
    static Publication EnsurePublicationBeforeCopy(RegionInfo* region, MAddress from);
    // After-copy consumer borrows that same installed forwarding.
    static Publication RetainOpenPublicationAfterCopy(RegionInfo* region, MAddress from);
    static ZForwarding::Receipt InstallMapping(const Publication& publication, MAddress from, MAddress to);
    static MAddress InsertMapping(const Publication& publication, MAddress from, MAddress to);
    // Out of line so the unit runner exercises the product SO's publication
    // decision instead of compiling a private test copy.
    static bool ReceiptAllowsForwarded(MAddress mapped);
    static uint64_t StaleToLifeCount();
    static MAddress FindTo(MAddress from);
    static bool EntriesArmed(MAddress from);
    static LookupResult LookupTo(MAddress from);
    // Fail-closed diagnostic only: enumerate all live carriers which cover the
    // target and, conditionally needed for an already-to target, reverse-scan
    // existing receipts.  This does not retain, publish, retire or destroy.
    static NeverInstalledSnapshot CaptureNeverInstalledSnapshot(MAddress target);
    static uint64_t ArmedHitCount();
    static uint64_t ArmedMissCount();
    static uint64_t UnarmedCount();



    static void NoteCompare(MAddress addr, bool legacy);
    static void NoteDestCompare(MAddress from, MAddress geometricTo);
    static void DumpCompare(const char* why);

    static bool Ready();

private:
    static size_t ObjectCountUpperBound(RegionInfo* region, size_t regionSize);
    static bool UnbindPageOwnerLocked(RegionInfo* region, bool allowExclusive);
};
} // namespace MapleRuntime

#endif // MRT_FORWARDING_TABLE_H
