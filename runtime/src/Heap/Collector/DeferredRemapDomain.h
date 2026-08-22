// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_DEFERRED_REMAP_DOMAIN_H
#define MRT_DEFERRED_REMAP_DOMAIN_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Common/TypeDef.h"
#include "Heap/Collector/RegionLifeClock.h"

namespace MapleRuntime {
class BaseObject;

// Durable old-holder obligations for references into an armed/retired from page.
// This is deliberately separate from RememberedSet: the remset answers a generation
// question, while this domain keeps a forwarding-table lifetime obligation alive.
//
// ZGC anchors (jdk@5b2d6991a): containing-field transfer zRelocate.cpp:652-730,
// publish zRelocate.cpp:1018-1020 + zForwarding.cpp:275-282, consume/heal
// zRemembered.cpp:284-321,50-78, and O(1) face flip zRememberedSet.cpp:34-38.
namespace DeferredRemapDomain {

enum class Producer : uint8_t {
    OldMark = 0,
    HolderMove,
    WriteBarrier,
    Count,
};

struct Record {
    MAddress slot = 0;
    MAddress holderStart = 0;
    RegionLifeId holderLife = 0;
    MAddress targetFrom = 0;
    MAddress targetFromStart = 0;
    RegionLifeId targetFromLife = 0;
    uint64_t birthMinor = 0;
    Producer producer = Producer::OldMark;
};

enum class ConsumeResult : uint8_t {
    Complete,
    Requeue,
    // Shadow-only: forwarding is published and a product consume would heal, but
    // this run must not write the slot. Kept for postflip fixed-slot parity.
    SimulatedHeal,
};

using Consumer = std::function<ConsumeResult(const Record&)>;
using SlotResolver = std::function<BaseObject*(MAddress)>;
using SlotRecorder = std::function<void(MAddress)>;

class OldMarkCaptureScope {
public:
    OldMarkCaptureScope(MAddress slot, bool enabled) : slot(slot), enabled(enabled) {}
    ~OldMarkCaptureScope();

    OldMarkCaptureScope(const OldMarkCaptureScope&) = delete;
    OldMarkCaptureScope& operator=(const OldMarkCaptureScope&) = delete;

private:
    MAddress slot;
    bool enabled;
};

struct Snapshot {
    uint64_t staged = 0;
    uint64_t captured = 0;
    uint64_t capturedByProducer[static_cast<size_t>(Producer::Count)]{};
    uint64_t retainFailed = 0;
    uint64_t duplicate = 0;
    uint64_t requeue = 0;
    uint64_t lifeMismatch = 0;
    uint64_t holderDead = 0;
    uint64_t consumed = 0;
    uint64_t simulatedHeal = 0;
    uint64_t missing = 0;
    uint64_t extra = 0;
    uint64_t oracleTracked = 0;
    uint64_t oracleFixed = 0;
    uint64_t postflipObserved = 0;
    uint64_t postflipObservedFixed = 0;
    uint64_t peak = 0;
    uint64_t maxMinorAge = 0;
    uint64_t flips = 0;
    uint64_t current = 0;
    uint64_t previous = 0;
    uint64_t awaitingOracle = 0;
    uint64_t capacity = 0;
    uint64_t postflipRounds = 0;
    uint64_t candidates = 0;
    uint64_t trackedByHolderType[16]{};
    uint64_t staleColorObserved = 0;
    uint64_t staleColorUnhealed = 0;
    uint64_t staleColorNoReceipt = 0;
    uint64_t walkSkipTl = 0;
    uint64_t walkSkipRecentFull = 0;
    uint64_t walkSkipOther = 0;
    uint64_t walkBreakHole = 0;
};

// REMAPDOMAIN_AUDIT=1 enables the zero-product-change shadow/oracle path.
bool AuditEnabled();
// Stage 2 is default off. It is the only gate that authorizes a consumer to heal.
bool ProductEnabled();
bool Active();

// Producer shared by old-mark field visits and the mutator write barrier. The
// target must still be covered by an armed or retired forwarding table. Capture
// holds RegionInfo::RetainScope and rechecks life/table state before publishing.
bool Capture(MAddress slot, BaseObject* target, Producer producer);

// Old marking precedes relocation-set installation in this collector. Stage
// live old-holder fields during the existing mark visit, then promote only
// those whose target page was actually selected once forwarding is armed.
bool StageOldMarkCandidate(MAddress slot, BaseObject* target);
bool StageYoungHolderRegion(MAddress slot);
bool StagePromotedHolderCandidate(MAddress slot, BaseObject* target);
size_t PublishOldMarkCandidates();

// Move obligations with a copied/compacted holder by the exact field offset.
size_t TransferObjectSlots(MAddress fromBase, MAddress toBase, size_t size);
size_t CaptureHolderFields(BaseObject* holder);

// Holder-death proof. Called before the holder region's remset/life is scrubbed.
size_t DischargeHolderRegion(MAddress start, MAddress end, RegionLifeId life);

// Young mark-start face rotation followed by mark-follow consumption.
void FlipForYoung(uint64_t minorIndex);
size_t ConsumePrevious(const Consumer& consumer);
size_t ConsumeYoungPrevious(const SlotResolver& resolver, const SlotRecorder& recorder);

// Postflip remains the product oracle. Begin/Note/End can be called from the
// parallel walk; Note is internally serialized. target is the pre-heal value.
void BeginPostflip(uint64_t majorIndex);
void NotePostflipSlot(MAddress slot, BaseObject* holder, BaseObject* target, uint8_t holderType, bool changed,
                      uintptr_t rawWord = 0, bool oldTagged = false);
// walkRange skip / VisitAllObjects hole. reason is a stable token, not a format string.
void NoteWalkSkip(MAddress regionStart, uint8_t regionType, uint8_t route, RegionLifeId life, MAddress allocPtr,
                  MAddress regionEnd, const char* reason);
void EndPostflip();

Snapshot GetSnapshot();
void Report(const char* point);

// Deterministic gc_unit seam. It bypasses env/table discovery, never product GC.
void ResetForTesting(size_t capacity);
bool InsertForTesting(const Record& record);

} // namespace DeferredRemapDomain
} // namespace MapleRuntime

#endif // MRT_DEFERRED_REMAP_DOMAIN_H
