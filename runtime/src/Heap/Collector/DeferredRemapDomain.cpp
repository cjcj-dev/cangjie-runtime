// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/DeferredRemapDomain.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace DeferredRemapDomain {
namespace {

using Face = std::unordered_map<MAddress, Record>;

// Exact slot lists per holder region for one face. Object copies ask
// "what does this source region hold" and touch only that list, instead of
// scanning every staged obligation (nw256 stages millions per major).
struct TicketIndex {
    std::unordered_map<MAddress, std::vector<MAddress>> byHolder;

    void Add(MAddress holderStart, MAddress slot)
    {
        if (holderStart != 0) {
            byHolder[holderStart].push_back(slot);
        }
    }

    void Remove(MAddress holderStart, MAddress slot)
    {
        auto found = byHolder.find(holderStart);
        if (found == byHolder.end()) {
            return;
        }
        auto& slots = found->second;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] == slot) {
                std::swap(slots[i], slots.back());
                slots.pop_back();
                break;
            }
        }
        if (slots.empty()) {
            byHolder.erase(found);
        }
    }
};

struct State {
    std::mutex mutex;
    Face current;
    Face previous;
    std::vector<Record> oldMarkCandidates;
    Face promotedCandidates;
    std::unordered_map<MAddress, RegionLifeId> youngHolderRegions;
    // Shadow-consumed records stay here until the unchanged product postflip
    // walk says which slots it actually fixed.
    Face awaitingOracle;
    std::unordered_set<MAddress> oracleTrackedSlots;
    std::unordered_set<MAddress> oracleFixed;
    std::unordered_map<MAddress, MAddress> oracleTargets;
    std::unordered_map<MAddress, uint8_t> publishOutcome;
    Snapshot stats;
    size_t capacity = 1U << 22; // hard upper bound, configurable before first use
    uint64_t currentMinor = 0;
    uint64_t currentMajor = 0;
    uint64_t roundTracked = 0;
    uint64_t roundFixed = 0;
    uint64_t roundObserved = 0;
    uint64_t roundObservedFixed = 0;
    uint64_t roundTrackedByHolderType[16]{};
    // Exact per-holder-region obligation counts over the four transferable
    // faces (current/previous/awaitingOracle/promotedCandidates). CopyCollector
    // faces millions of object copies per major; without this index each copy
    // linearly scanned every face (measured: nw256 never finished a second
    // major). A count of zero for the source region makes TransferObjectSlots
    // an O(1) skip; a nonzero count still scans exactly.
    TicketIndex currentIdx;
    TicketIndex previousIdx;
    TicketIndex awaitingIdx;
    TicketIndex promotedIdx;
    bool postflipOpen = false;
    bool testing = false;
};

State& Domain()
{
    static State state;
    return state;
}

bool EnvOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t ConfiguredCapacity()
{
    constexpr size_t kDefault = 1U << 22;
    constexpr size_t kMax = 1U << 24;
    const char* value = std::getenv("REMAPDOMAIN_CAPACITY");
    if (value == nullptr || value[0] == '\0') {
        return kDefault;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > kMax) {
        CHECK_DETAIL(false,
                     "DEFERRED_REMAP_DOMAIN_CAPACITY_FATAL invalid REMAPDOMAIN_CAPACITY=%s range=[1,%zu]",
                     value, kMax);
    }
    return static_cast<size_t>(parsed);
}

const char* ProducerName(Producer producer)
{
    switch (producer) {
        case Producer::OldMark:
            return "old_mark";
        case Producer::HolderMove:
            return "holder_move";
        case Producer::WriteBarrier:
            return "write_barrier";
        default:
            return "unknown";
    }
}

size_t ObligationsUnlocked(const State& state)
{
    return state.current.size() + state.previous.size() + state.awaitingOracle.size() +
        state.oldMarkCandidates.size() + state.promotedCandidates.size() + state.youngHolderRegions.size();
}

void UpdatePeakUnlocked(State& state)
{
    const uint64_t held = ObligationsUnlocked(state);
    state.stats.peak = std::max(state.stats.peak, held);
}

void DropFaceRecordUnlocked(State& state, Face& face, TicketIndex& index, MAddress slot)
{
    auto found = face.find(slot);
    if (found != face.end()) {
        index.Remove(found->second.holderStart, slot);
        face.erase(found);
    }
    (void)state;
}

void CapacityFatalUnlocked(const State& state, MAddress slot, Producer producer)
{
    CHECK_DETAIL(false,
                 "DEFERRED_REMAP_DOMAIN_CAPACITY_FATAL slot=%#zx producer=%s held=%zu capacity=%zu "
                 "set REMAPDOMAIN_CAPACITY (max=%u)",
                 static_cast<size_t>(slot), ProducerName(producer), ObligationsUnlocked(state), state.capacity,
                 1U << 24);
}

bool IsTrackedTarget(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    const MAddress address = reinterpret_cast<MAddress>(target);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(address);
    if (region == nullptr) {
        return false;
    }
    return ForwardingTable::GetEntries(address) != nullptr ||
        ForwardingTable::RetiredCovers(region->GetRegionStart(), region->GetRegionSize());
}

bool IsSelectedTarget(BaseObject* target)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    return region != nullptr &&
        (region->IsFromRegion() || region->IsUnmovableFromRegion() ||
         region->GetRouteState() != RegionInfo::RouteState::NORMAL || IsTrackedTarget(target));
}

void NoteAgeUnlocked(State& state, const Record& record)
{
    const uint64_t age = state.currentMinor >= record.birthMinor ? state.currentMinor - record.birthMinor : 0;
    state.stats.maxMinorAge = std::max(state.stats.maxMinorAge, age);
}

// A new store wins over an older ticket for the same slot. The old obligation
// is complete because the slot no longer contains its recorded target.
void EraseOlderFacesUnlocked(State& state, MAddress slot)
{
    DropFaceRecordUnlocked(state, state.previous, state.previousIdx, slot);
    DropFaceRecordUnlocked(state, state.awaitingOracle, state.awaitingIdx, slot);
}

bool InsertCurrentUnlocked(State& state, Record record, bool countCapture)
{
    auto found = state.current.find(record.slot);
    if (found != state.current.end()) {
        found->second = record;
        return false;
    }
    EraseOlderFacesUnlocked(state, record.slot);
    if (ObligationsUnlocked(state) >= state.capacity) {
        CapacityFatalUnlocked(state, record.slot, record.producer);
    }
    state.current.emplace(record.slot, record);
    state.currentIdx.Add(record.holderStart, record.slot);
    if (countCapture) {
        ++state.stats.captured;
        ++state.stats.capturedByProducer[static_cast<size_t>(record.producer)];
    }
    UpdatePeakUnlocked(state);
    return true;
}

void EnsureAtexit()
{
    static std::atomic<bool> installed{ false };
    bool expected = false;
    if (installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

template<class Predicate>
size_t EraseIf(State& state, Face& face, TicketIndex& index, const Predicate& predicate)
{
    size_t erased = 0;
    for (auto it = face.begin(); it != face.end();) {
        if (predicate(it->second)) {
            index.Remove(it->second.holderStart, it->first);
            it = face.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
}

} // namespace

bool AuditEnabled()
{
    static const bool enabled = EnvOne("REMAPDOMAIN_AUDIT");
    if (enabled) {
        State& state = Domain();
        static std::once_flag configured;
        std::call_once(configured, [&state]() { state.capacity = ConfiguredCapacity(); });
        EnsureAtexit();
    }
    return enabled;
}

bool ProductEnabled()
{
    static const bool enabled = EnvOne("CJRT_REMAP_DOMAIN");
    if (enabled) {
        State& state = Domain();
        static std::once_flag configured;
        std::call_once(configured, [&state]() { state.capacity = ConfiguredCapacity(); });
        EnsureAtexit();
    }
    return enabled;
}

bool Active() { return AuditEnabled() || ProductEnabled(); }

OldMarkCaptureScope::~OldMarkCaptureScope()
{
    if (!enabled) {
        return;
    }
    RefField<>& field = HeapSlotAt<>(slot);
    RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
    // A live young holder visited by a full-GC mark becomes an old holder in
    // this same relocation.  It is therefore already an old-side obligation
    // at the publication fence, even though its generation bit is not changed
    // until the later copy/promote arm.  Coalesce at region granularity until
    // selection is known; CopyObject later transfers tickets by field offset.
    if (holder != nullptr && holder->IsYoungRegion()) {
        (void)StageYoungHolderRegion(slot);
    } else {
        (void)StageOldMarkCandidate(slot, to_object(field.GetTargetObject()));
    }
}

bool Capture(MAddress slot, BaseObject* target, Producer producer)
{
    if (!Active() || slot == 0 || !Heap::IsHeapAddress(slot) || target == nullptr ||
        !Heap::IsHeapAddress(target)) {
        return false;
    }
    // Observe the installed value, not the barrier's pre-resolution argument.
    // If the write path already healed, this turns Capture into a no-op.
    target = to_object(HeapSlotAt<>(slot).GetTargetObject());
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (holder == nullptr || holder->IsYoungRegion() || holder->IsFreeRegion() || holder->IsGarbageRegion() ||
        targetRegion == nullptr || !IsTrackedTarget(target)) {
        return false;
    }

    const MAddress holderStart = holder->GetRegionStart();
    const RegionLifeId holderLife = holder->GetRegionLifeId();
    const RegionLifeId targetLife = targetRegion->GetRegionLifeId();
    RegionInfo::RetainScope retain(targetRegion);
    if (!retain.ok()) {
        State& state = Domain();
        std::lock_guard<std::mutex> guard(state.mutex);
        ++state.stats.retainFailed;
        return false;
    }
    // Capture owns a retain across both reads. A failed recheck means another
    // path already healed/published the obligation; it must not mint a stale ticket.
    BaseObject* installed = to_object(HeapSlotAt<>(slot).GetTargetObject());
    holder = RegionInfo::TryGetRegionInfoAt(holderStart);
    if (reinterpret_cast<MAddress>(installed) != reinterpret_cast<MAddress>(target) ||
        holder == nullptr || holder->GetRegionLifeId() != holderLife ||
        targetLife != targetRegion->GetRegionLifeId() || !IsTrackedTarget(target)) {
        return false;
    }

    Record record;
    record.slot = slot;
    record.holderStart = holderStart;
    record.holderLife = holderLife;
    record.targetFrom = reinterpret_cast<MAddress>(target);
    record.targetFromStart = targetRegion->GetRegionStart();
    record.targetFromLife = targetLife;
    record.producer = producer;

    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    record.birthMinor = state.currentMinor;
    return InsertCurrentUnlocked(state, record, true);
}

static bool StageCandidate(MAddress slot, BaseObject* target, bool allowYoungHolder, Producer producer)
{
    if (!Active() || slot == 0 || !Heap::IsHeapAddress(slot) || target == nullptr ||
        !Heap::IsHeapAddress(target)) {
        return false;
    }
    target = to_object(HeapSlotAt<>(slot).GetTargetObject());
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (holder == nullptr || (!allowYoungHolder && holder->IsYoungRegion()) || holder->IsFreeRegion() ||
        holder->IsGarbageRegion() || targetRegion == nullptr || targetRegion->IsFreeRegion() ||
        targetRegion->IsGarbageRegion()) {
        return false;
    }
    if (allowYoungHolder && targetRegion->IsYoungRegion()) {
        return false;
    }

    Record record;
    record.slot = slot;
    record.holderStart = holder->GetRegionStart();
    record.holderLife = holder->GetRegionLifeId();
    record.targetFrom = reinterpret_cast<MAddress>(target);
    record.targetFromStart = targetRegion->GetRegionStart();
    record.targetFromLife = targetRegion->GetRegionLifeId();
    record.producer = producer;

    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    record.birthMinor = state.currentMinor;
    if (allowYoungHolder) {
        auto found = state.promotedCandidates.find(slot);
        if (found != state.promotedCandidates.end()) {
            found->second = record;
        } else {
            if (ObligationsUnlocked(state) >= state.capacity) {
                CapacityFatalUnlocked(state, slot, producer);
            }
            state.promotedCandidates.emplace(slot, record);
            state.promotedIdx.Add(record.holderStart, slot);
        }
    } else {
        if (ObligationsUnlocked(state) >= state.capacity) {
            CapacityFatalUnlocked(state, slot, producer);
        }
        if (state.oldMarkCandidates.capacity() == 0) {
            state.oldMarkCandidates.reserve(std::min(state.capacity, static_cast<size_t>(1U << 20)));
        }
        state.oldMarkCandidates.push_back(record);
    }
    if (AuditEnabled()) {
        state.publishOutcome[slot] = 1; // staged
    }
    ++state.stats.staged;
    UpdatePeakUnlocked(state);
    return true;
}

bool StageOldMarkCandidate(MAddress slot, BaseObject* target)
{
    return StageCandidate(slot, target, false, Producer::OldMark);
}

bool StageYoungHolderRegion(MAddress slot)
{
    if (!Active() || slot == 0 || !Heap::IsHeapAddress(slot)) {
        return false;
    }
    RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
    if (holder == nullptr || !holder->IsYoungRegion() || holder->IsFreeRegion() || holder->IsGarbageRegion()) {
        return false;
    }
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    const MAddress start = holder->GetRegionStart();
    auto found = state.youngHolderRegions.find(start);
    if (found != state.youngHolderRegions.end()) {
        found->second = holder->GetRegionLifeId();
        return false;
    }
    if (ObligationsUnlocked(state) >= state.capacity) {
        CapacityFatalUnlocked(state, slot, Producer::HolderMove);
    }
    state.youngHolderRegions.emplace(start, holder->GetRegionLifeId());
    UpdatePeakUnlocked(state);
    return true;
}

bool StagePromotedHolderCandidate(MAddress slot, BaseObject* target)
{
    return StageCandidate(slot, target, true, Producer::HolderMove);
}

size_t PublishOldMarkCandidates()
{
    if (!Active()) {
        return 0;
    }
    State& state = Domain();
    const bool audit = AuditEnabled();
    std::vector<Record> candidates;
    std::unordered_map<MAddress, RegionLifeId> youngRegions;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        candidates.reserve(state.oldMarkCandidates.size() + state.promotedCandidates.size());
        candidates.swap(state.oldMarkCandidates);
        for (const auto& item : state.promotedCandidates) {
            candidates.push_back(item.second);
        }
        state.promotedCandidates.clear();
        state.promotedIdx.byHolder.clear();
        youngRegions.swap(state.youngHolderRegions);
    }

    // Full-GC live young holders are promoted later in this same relocation.
    // Keep only their selected-target fields at the publication fence.  The
    // region-level shadow is bounded and filler makes this dense scan complete,
    // avoiding an unbounded per-field young graph before selection is known.
    for (const auto& item : youngRegions) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(item.first);
        if (region == nullptr || region->GetRegionLifeId() != item.second || region->IsFreeRegion() ||
            region->IsGarbageRegion()) {
            continue;
        }
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->VisitAllObjects([&](BaseObject* object) {
            if (object == nullptr || !object->HasRefField() ||
                !region->IsSurvivedObject(view, region->GetAddressOffset(reinterpret_cast<MAddress>(object)))) {
                return;
            }
            object->ForEachRefField([&](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (!IsSelectedTarget(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion == nullptr) {
                    return;
                }
                if (candidates.size() >= state.capacity) {
                    CHECK_DETAIL(false,
                                 "DEFERRED_REMAP_DOMAIN_CAPACITY_FATAL slot=%#zx producer=holder_move "
                                 "publicationCandidates=%zu capacity=%zu",
                                 static_cast<size_t>(reinterpret_cast<MAddress>(&field)), candidates.size(),
                                 state.capacity);
                }
                Record record;
                record.slot = reinterpret_cast<MAddress>(&field);
                record.holderStart = region->GetRegionStart();
                record.holderLife = region->GetRegionLifeId();
                record.targetFrom = reinterpret_cast<MAddress>(target);
                record.targetFromStart = targetRegion->GetRegionStart();
                record.targetFromLife = targetRegion->GetRegionLifeId();
                record.producer = Producer::HolderMove;
                candidates.push_back(record);
            });
        });
    }

    size_t published = 0;
    size_t holderGone = 0;
    size_t holderLifeMismatch = 0;
    size_t targetGone = 0;
    size_t targetLifeMismatch = 0;
    size_t slotChanged = 0;
    size_t targetNotSelected = 0;
    size_t retainFailed = 0;
    size_t recheckFailed = 0;
    size_t targetNotTracked = 0;
    // Per-target-region memos: forwarding coverage and retainability are region
    // properties, and a full-GC publish faces millions of candidates that share
    // a few hundred regions. Without the memos the shadow burns one RetainScope
    // per candidate (measured 4.95M retainFailed on nw256) and cannot finish a
    // major inside the product wall.
    std::unordered_map<MAddress, uint8_t> trackedMemo;
    std::unordered_map<MAddress, uint8_t> retainMemo;
    auto trackedCached = [&trackedMemo](BaseObject* target) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region == nullptr) {
            return false;
        }
        const MAddress start = region->GetRegionStart();
        auto found = trackedMemo.find(start);
        if (found == trackedMemo.end()) {
            const bool value = IsTrackedTarget(target);
            found = trackedMemo.emplace(start, value ? 1 : 2).first;
        }
        return found->second == 1;
    };
    auto retainCached = [&retainMemo](RegionInfo* region) {
        const MAddress start = region->GetRegionStart();
        auto found = retainMemo.find(start);
        if (found == retainMemo.end()) {
            RegionInfo::RetainScope retain(region);
            const bool ok = retain.ok();
            found = retainMemo.emplace(start, ok ? 1 : 2).first;
            if (!ok) {
                return false;
            }
        }
        return found->second == 1;
    };
    for (Record& record : candidates) {
        RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(record.holderStart);
        if (holder == nullptr || holder->IsFreeRegion() || holder->IsGarbageRegion()) {
            if (audit) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.publishOutcome[record.slot] = 3; // holder gone
            }
            ++holderGone;
            continue;
        }
        if (holder->GetRegionLifeId() != record.holderLife) {
            if (audit) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.publishOutcome[record.slot] = 4; // holder life
            }
            ++holderLifeMismatch;
            continue;
        }
        BaseObject* observed = to_object(HeapSlotAt<>(record.slot).GetTargetObject());
        if (reinterpret_cast<MAddress>(observed) != record.targetFrom) {
            ++slotChanged;
            // Mark-follow can heal this field after its holder was visited.  The
            // slot/holder ticket is still valid, but its target obligation is
            // latest-wins just like a mutator store.  Re-snapshot at the
            // relocation-set publication fence instead of minting a stale duty.
            if (observed == nullptr || !Heap::IsHeapAddress(observed)) {
                continue;
            }
            RegionInfo* latestRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(observed));
            if (latestRegion == nullptr || latestRegion->IsFreeRegion() || latestRegion->IsGarbageRegion()) {
                ++targetGone;
                continue;
            }
            record.targetFrom = reinterpret_cast<MAddress>(observed);
            record.targetFromStart = latestRegion->GetRegionStart();
            record.targetFromLife = latestRegion->GetRegionLifeId();
        }
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(record.targetFromStart);
        if (targetRegion == nullptr || targetRegion->IsFreeRegion() || targetRegion->IsGarbageRegion()) {
            ++targetGone;
            continue;
        }
        if (targetRegion->GetRegionLifeId() != record.targetFromLife) {
            ++targetLifeMismatch;
            continue;
        }
        if (!IsSelectedTarget(observed) || !trackedCached(observed)) {
            if (audit) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.publishOutcome[record.slot] = IsSelectedTarget(observed) ? 7 : 5; // not selected / not tracked
            }
            if (!trackedCached(observed)) {
                ++targetNotTracked;
            } else {
                ++targetNotSelected;
            }
            continue;
        }
        if (!retainCached(targetRegion)) {
            std::lock_guard<std::mutex> guard(state.mutex);
            ++state.stats.retainFailed;
            if (audit) {
                state.publishOutcome[record.slot] = 6; // retain failed
            }
            ++retainFailed;
            continue;
        }
        RegionInfo::RetainScope retain(targetRegion);
        if (!retain.ok()) {
            std::lock_guard<std::mutex> guard(state.mutex);
            ++state.stats.retainFailed;
            if (audit) {
                state.publishOutcome[record.slot] = 6; // retain failed
            }
            ++retainFailed;
            continue;
        }
        observed = to_object(HeapSlotAt<>(record.slot).GetTargetObject());
        if (targetRegion->GetRegionLifeId() != record.targetFromLife ||
            reinterpret_cast<MAddress>(observed) != record.targetFrom || !IsSelectedTarget(observed)) {
            ++recheckFailed;
            continue;
        }
        std::lock_guard<std::mutex> guard(state.mutex);
        record.birthMinor = state.currentMinor;
        if (InsertCurrentUnlocked(state, record, true)) {
            ++published;
        }
        if (audit) {
            state.publishOutcome[record.slot] = 2; // published or coalesced into current
        }
    }
    if (audit && !state.testing && !candidates.empty()) {
        std::fprintf(stderr,
                     "[REMAPDOMAIN][publish] candidates=%zu published=%zu holderGone=%zu holderLife=%zu "
                     "targetGone=%zu targetLife=%zu slotChanged=%zu notSelected=%zu notTracked=%zu "
                     "retainFailed=%zu recheckFailed=%zu\n",
                     candidates.size(), published, holderGone, holderLifeMismatch, targetGone,
                     targetLifeMismatch, slotChanged, targetNotSelected, targetNotTracked,
                     retainFailed, recheckFailed);
    }
    return published;
}

size_t TransferObjectSlots(MAddress fromBase, MAddress toBase, size_t size)
{
    if ((!Active() && !Domain().testing) || size == 0 || fromBase == toBase || !Heap::IsHeapAddress(fromBase) ||
        !Heap::IsHeapAddress(toBase)) {
        return 0;
    }
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(toBase);
    if (toRegion == nullptr) {
        return 0;
    }
    const MAddress fromEnd = fromBase + size;
    const ptrdiff_t delta = static_cast<ptrdiff_t>(toBase) - static_cast<ptrdiff_t>(fromBase);
    State& state = Domain();
    size_t moved = 0;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        // O(1) skip: tickets are keyed by their holder, and a copied object's
        // slots always live in the source object's region. No obligations for
        // that region means nothing to move for any object copied out of it.
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(fromBase);
        if (fromRegion == nullptr) {
            return 0;
        }
        const MAddress fromRegionStart = fromRegion->GetRegionStart();
        const MAddress toRegionStart = toRegion->GetRegionStart();
        const MAddress toRegionLife = toRegion->GetRegionLifeId();
        auto holderAny = [&](const TicketIndex& index) {
            return index.byHolder.find(fromRegionStart) != index.byHolder.end();
        };
        if (!holderAny(state.currentIdx) && !holderAny(state.previousIdx) && !holderAny(state.awaitingIdx) &&
            !holderAny(state.promotedIdx)) {
            return 0;
        }
        auto transferFace = [&](Face& face, TicketIndex& index) {
            auto held = index.byHolder.find(fromRegionStart);
            if (held == index.byHolder.end()) {
                return;
            }
            std::vector<MAddress> entries;
            entries.swap(held->second);
            index.byHolder.erase(held);
            std::vector<MAddress> stale;
            for (MAddress slot : entries) {
                auto found = face.find(slot);
                if (found == face.end() || found->first < fromBase || found->first >= fromEnd) {
                    // Stale list entry (the record was replaced or dropped by a
                    // same-slot newer store after this snapshot was taken).
                    if (found != face.end()) {
                        stale.push_back(slot);
                    }
                    continue;
                }
                Record record = found->second;
                record.slot = static_cast<MAddress>(static_cast<ptrdiff_t>(found->first) + delta);
                record.holderStart = toRegionStart;
                record.holderLife = toRegionLife;
                record.producer = Producer::HolderMove;
                face.erase(found);
                auto inserted = face.emplace(record.slot, record);
                if (inserted.second) {
                    index.Add(toRegionStart, record.slot);
                } else {
                    inserted.first->second = record;
                }
                ++moved;
                ++state.stats.capturedByProducer[static_cast<size_t>(Producer::HolderMove)];
            }
            // Entries skipped as out-of-range stay with their original holder.
            for (MAddress slot : stale) {
                index.Add(fromRegionStart, slot);
            }
        };
        transferFace(state.current, state.currentIdx);
        transferFace(state.previous, state.previousIdx);
        transferFace(state.awaitingOracle, state.awaitingIdx);
        transferFace(state.promotedCandidates, state.promotedIdx);
        UpdatePeakUnlocked(state);
    }

    return moved;
}

size_t CaptureHolderFields(BaseObject* holder)
{
    if (!Active() || holder == nullptr || !Heap::IsHeapAddress(holder) || !holder->HasRefField()) {
        return 0;
    }
    size_t captured = 0;
    holder->ForEachRefField([&captured](RefField<>& field) {
        if (Capture(reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()), Producer::HolderMove)) {
            ++captured;
        }
    });
    return captured;
}

size_t DischargeHolderRegion(MAddress start, MAddress end, RegionLifeId life)
{
    if ((!Active() && !Domain().testing) || start >= end) {
        return 0;
    }
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    auto belongsToDeadLife = [start, end, life](const Record& record) {
        return record.slot >= start && record.slot < end && record.holderStart == start &&
            record.holderLife == life;
    };
    size_t discharged = EraseIf(state, state.current, state.currentIdx, belongsToDeadLife);
    discharged += EraseIf(state, state.previous, state.previousIdx, belongsToDeadLife);
    discharged += EraseIf(state, state.awaitingOracle, state.awaitingIdx, belongsToDeadLife);
    discharged += EraseIf(state, state.promotedCandidates, state.promotedIdx, belongsToDeadLife);
    state.stats.holderDead += discharged;
    return discharged;
}

void FlipForYoung(uint64_t minorIndex)
{
    if (!Active() && !Domain().testing) {
        return;
    }
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    CHECK_DETAIL(state.previous.empty(),
                 "DEFERRED_REMAP_DOMAIN_PREVIOUS_NOT_EMPTY_FATAL previous=%zu current=%zu minor=%llu",
                 state.previous.size(), state.current.size(), static_cast<unsigned long long>(minorIndex));
    state.currentMinor = minorIndex;
    state.previous.swap(state.current);
    state.previousIdx.byHolder.swap(state.currentIdx.byHolder);
    ++state.stats.flips;
}

size_t ConsumePrevious(const Consumer& consumer)
{
    if ((!Active() && !Domain().testing) || !consumer) {
        return 0;
    }
    State& state = Domain();
    std::vector<Record> records;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        records.reserve(state.previous.size());
        for (const auto& item : state.previous) {
            records.push_back(item.second);
        }
        state.previous.clear();
        state.previousIdx.byHolder.clear();
    }

    for (Record& record : records) {
        ConsumeResult result = ConsumeResult::Requeue;
        RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(record.holderStart);
        RegionInfo* target = RegionInfo::TryGetRegionInfoAt(record.targetFromStart);
        bool holderDead = holder == nullptr || holder->IsFreeRegion() || holder->IsGarbageRegion();
        bool lifeMismatch = !holderDead && holder->GetRegionLifeId() != record.holderLife;
        lifeMismatch = lifeMismatch || target == nullptr || target->GetRegionLifeId() != record.targetFromLife;

        if (holderDead) {
            result = ConsumeResult::Complete;
        } else if (lifeMismatch) {
            result = ConsumeResult::Requeue;
        } else {
            RegionInfo::RetainScope retain(target);
            if (!retain.ok()) {
                std::lock_guard<std::mutex> guard(state.mutex);
                ++state.stats.retainFailed;
                result = ConsumeResult::Requeue;
            } else {
                result = consumer(record);
            }
        }

        std::lock_guard<std::mutex> guard(state.mutex);
        NoteAgeUnlocked(state, record);
        if (holderDead) {
            ++state.stats.holderDead;
            ++state.stats.consumed;
            continue;
        }
        if (lifeMismatch) {
            ++state.stats.lifeMismatch;
        }
        if (result == ConsumeResult::Complete) {
            ++state.stats.consumed;
        } else if (result == ConsumeResult::SimulatedHeal) {
            ++state.stats.simulatedHeal;
            auto inserted = state.awaitingOracle.emplace(record.slot, record);
            if (inserted.second) {
                state.awaitingIdx.Add(record.holderStart, record.slot);
            } else {
                inserted.first->second = record;
            }
        } else {
            ++state.stats.requeue;
            // A mutator may already have installed a newer current-face ticket.
            auto inserted = state.current.emplace(record.slot, record);
            if (inserted.second) {
                state.currentIdx.Add(record.holderStart, record.slot);
            }
            (void)inserted;
        }
        UpdatePeakUnlocked(state);
    }
    return records.size();
}

size_t ConsumeYoungPrevious(const SlotResolver& resolver, const SlotRecorder& recorder)
{
    return ConsumePrevious([&resolver, &recorder](const Record& record) {
        if (!Heap::IsHeapAddress(record.slot)) {
            return ConsumeResult::Requeue;
        }
        RefField<>& field = HeapSlotAt<>(record.slot);
        BaseObject* observed = to_object(field.GetTargetObject());
        if (reinterpret_cast<MAddress>(observed) != record.targetFrom) {
            // A mutator overwrite or an earlier heal discharged this ticket.
            return ConsumeResult::Complete;
        }
        const MAddress toAddress = ForwardingTable::FindTo(record.targetFrom);
        if (toAddress == 0) {
            // Copy receipt is not published. Never count this as complete.
            return ConsumeResult::Requeue;
        }
        if (!ProductEnabled()) {
            return ConsumeResult::SimulatedHeal;
        }
        if (!resolver) {
            return ConsumeResult::Requeue;
        }
        BaseObject* resolved = resolver(record.slot);
        BaseObject* installed = to_object(field.GetTargetObject());
        if (resolved == nullptr || reinterpret_cast<MAddress>(installed) == record.targetFrom) {
            return ConsumeResult::Requeue;
        }
        RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(installed));
        if (targetRegion != nullptr && targetRegion->IsYoungRegion() && recorder) {
            recorder(record.slot);
        }
        return ConsumeResult::Complete;
    });
}

void BeginPostflip(uint64_t majorIndex)
{
    if (!Active() && !Domain().testing) {
        return;
    }
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    CHECK_DETAIL(!state.postflipOpen, "DEFERRED_REMAP_DOMAIN_POSTFLIP_NESTED_FATAL major=%llu",
                 static_cast<unsigned long long>(majorIndex));
    state.currentMajor = majorIndex;
    state.roundTracked = 0;
    state.roundFixed = 0;
    state.roundObserved = 0;
    state.roundObservedFixed = 0;
    std::memset(state.roundTrackedByHolderType, 0, sizeof(state.roundTrackedByHolderType));
    state.oracleTrackedSlots.clear();
    state.oracleFixed.clear();
    state.oracleTargets.clear();
    state.postflipOpen = true;
}

void NotePostflipSlot(MAddress slot, BaseObject* holder, BaseObject* target, uint8_t holderType, bool changed)
{
    if ((!Active() && !Domain().testing) || slot == 0 || target == nullptr) {
        return;
    }
    State& state = Domain();
    if (!state.testing) {
        if (!IsTrackedTarget(target)) {
            return;
        }
        // The product walk also recolours old-tagged references whose object
        // never moved.  That is not a deferred-remap obligation: only an exact
        // forwarding receipt proves that this concrete from object requires a
        // to_addr+offset repair (zRemembered.cpp:50-78).
        ZForwarding* forwarding = ForwardingTable::GetEntries(reinterpret_cast<MAddress>(target));
        const MAddress to = forwarding == nullptr ? 0 : forwarding->find(reinterpret_cast<MAddress>(target));
        if (to == 0 || to == reinterpret_cast<MAddress>(target)) {
            return;
        }
    }
    std::lock_guard<std::mutex> guard(state.mutex);
    if (!state.postflipOpen) {
        return;
    }
    ++state.stats.postflipObserved;
    ++state.roundObserved;
    if (changed) {
        ++state.stats.postflipObservedFixed;
        ++state.roundObservedFixed;
    }
    bool liveHolder = true;
    if (!state.testing && holder != nullptr) {
        const MAddress holderFrom = reinterpret_cast<MAddress>(holder);
        ZForwarding* holderForwarding = ForwardingTable::GetEntries(holderFrom);
        const MAddress holderTo = holderForwarding == nullptr ? 0 : holderForwarding->find(holderFrom);
        if (holderTo != 0) {
            liveHolder = holderTo == holderFrom;
        } else {
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(holderFrom);
            if (holderRegion != nullptr &&
                (holderRegion->IsFromRegion() || holderRegion->IsLoneFromRegion() ||
                 holderRegion->IsUnmovableFromRegion() ||
                 holderRegion->GetRouteState() != RegionInfo::RouteState::NORMAL)) {
                MarkView<Generation::Old> view = holderRegion->GetMarkView<Generation::Old>();
                liveHolder = holderRegion->IsSurvivedObject(
                    view, holderRegion->GetAddressOffset(holderFrom));
            }
        }
    }
    if (state.oracleTrackedSlots.count(slot) == 0 && state.oracleTrackedSlots.size() >= state.capacity) {
        CHECK_DETAIL(false,
                     "DEFERRED_REMAP_DOMAIN_ORACLE_CAPACITY_FATAL slot=%#zx tracked=%zu capacity=%zu",
                     static_cast<size_t>(slot), state.oracleTrackedSlots.size(), state.capacity);
    }
    if (!liveHolder) {
        return;
    }
    const auto inserted = state.oracleTrackedSlots.insert(slot);
    if (!inserted.second) {
        ++state.stats.duplicate;
        return;
    }
    ++state.stats.oracleTracked;
    ++state.roundTracked;
    state.oracleTargets[slot] = reinterpret_cast<MAddress>(target);
    if (holderType < 16) {
        ++state.stats.trackedByHolderType[holderType];
        ++state.roundTrackedByHolderType[holderType];
    }
    if (changed) {
        ++state.stats.oracleFixed;
        ++state.roundFixed;
        state.oracleFixed.insert(slot);
    }
}

void EndPostflip()
{
    if (!Active() && !Domain().testing) {
        return;
    }
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    CHECK_DETAIL(state.postflipOpen, "DEFERRED_REMAP_DOMAIN_POSTFLIP_NOT_OPEN_FATAL");

    auto hasObligation = [&state](MAddress slot) {
        return state.current.count(slot) != 0 || state.previous.count(slot) != 0 ||
            state.awaitingOracle.count(slot) != 0;
    };
    uint64_t roundMissing = 0;
    for (MAddress slot : state.oracleFixed) {
        if (!hasObligation(slot)) {
            if (AuditEnabled() && !state.testing && roundMissing < 8) {
                const MAddress targetAddress = state.oracleTargets[slot];
                RegionInfo* holder = RegionInfo::TryGetRegionInfoAt(slot);
                RegionInfo* target = RegionInfo::TryGetRegionInfoAt(targetAddress);
                std::fprintf(stderr,
                             "[REMAPDOMAIN][missing] slot=%#zx holderStart=%#zx holderLife=%llu holderType=%u "
                             "target=%#zx targetStart=%#zx targetLife=%llu targetType=%u exactTo=%#zx\n",
                             static_cast<size_t>(slot),
                             static_cast<size_t>(holder == nullptr ? 0 : holder->GetRegionStart()),
                             static_cast<unsigned long long>(holder == nullptr ? 0 : holder->GetRegionLifeId()),
                             holder == nullptr ? 255U : static_cast<unsigned>(holder->GetRegionType()),
                             static_cast<size_t>(targetAddress),
                             static_cast<size_t>(target == nullptr ? 0 : target->GetRegionStart()),
                             static_cast<unsigned long long>(target == nullptr ? 0 : target->GetRegionLifeId()),
                             target == nullptr ? 255U : static_cast<unsigned>(target->GetRegionType()),
                             static_cast<size_t>(ForwardingTable::FindTo(targetAddress)));
                std::fprintf(stderr, "[REMAPDOMAIN][missing-outcome] slot=%#zx publishOutcome=%u\n",
                             static_cast<size_t>(slot), static_cast<unsigned>(state.publishOutcome[slot]));
            }
            ++roundMissing;
        }
    }
    uint64_t roundExtra = 0;
    std::vector<MAddress> shadowComplete;
    for (const auto& item : state.awaitingOracle) {
        BaseObject* installed = to_object(HeapSlotAt<>(item.first).GetTargetObject());
        if (!state.testing && reinterpret_cast<MAddress>(installed) != item.second.targetFrom) {
            shadowComplete.push_back(item.first);
        } else if (state.oracleFixed.count(item.first) == 0 &&
                   (state.testing || state.oracleTrackedSlots.count(item.first) != 0)) {
            ++roundExtra;
        } else if (!state.testing && state.oracleTrackedSlots.count(item.first) == 0) {
            // The audit-only young simulation proved an exact receipt at the
            // consume point. Product ON would have healed there; absence from a
            // later postflip walk means that temporal oracle no longer exists,
            // not that the obligation remained unfinished.
            shadowComplete.push_back(item.first);
        }
    }
    state.stats.missing += roundMissing;
    state.stats.extra += roundExtra;
    ++state.stats.postflipRounds;

    for (MAddress slot : state.oracleFixed) {
        DropFaceRecordUnlocked(state, state.current, state.currentIdx, slot);
        DropFaceRecordUnlocked(state, state.previous, state.previousIdx, slot);
        DropFaceRecordUnlocked(state, state.awaitingOracle, state.awaitingIdx, slot);
    }
    for (MAddress slot : shadowComplete) {
        DropFaceRecordUnlocked(state, state.awaitingOracle, state.awaitingIdx, slot);
        ++state.stats.consumed;
    }
    // Shadow simulation did not write product state. If the oracle did not fix
    // it either, the obligation remains live and must be retried next minor.
    for (const auto& item : state.awaitingOracle) {
        NoteAgeUnlocked(state, item.second);
        auto inserted = state.current.emplace(item.first, item.second);
        if (inserted.second) {
            state.currentIdx.Add(item.second.holderStart, item.first);
        }
        (void)inserted;
        ++state.stats.requeue;
    }
    state.awaitingOracle.clear();
    state.awaitingIdx.byHolder.clear();
    state.oracleTrackedSlots.clear();
    state.oracleTargets.clear();
    state.publishOutcome.clear();

    if (AuditEnabled() && !state.testing) {
        std::fprintf(stderr,
                     "[REMAPDOMAIN][postflip-audit] major=%llu exactTracked=%llu exactFixed=%llu "
                     "retiredHolderCopies=%llu\n",
                     static_cast<unsigned long long>(state.currentMajor),
                     static_cast<unsigned long long>(state.roundObserved),
                     static_cast<unsigned long long>(state.roundObservedFixed),
                     static_cast<unsigned long long>(state.roundObserved - state.roundTracked));
        std::fprintf(stderr,
                     "[REMAPDOMAIN][parity] major=%llu tracked=%zu fixed=%zu missing=%llu extra=%llu "
                     "duplicate=%llu requeue=%llu lifeMismatch=%llu maxMinorAge=%llu current=%zu previous=%zu "
                     "peak=%llu capacity=%zu product=%u\n",
                     static_cast<unsigned long long>(state.currentMajor),
                     static_cast<size_t>(state.roundTracked),
                     static_cast<size_t>(state.roundFixed), static_cast<unsigned long long>(roundMissing),
                     static_cast<unsigned long long>(roundExtra),
                     static_cast<unsigned long long>(state.stats.duplicate),
                     static_cast<unsigned long long>(state.stats.requeue),
                     static_cast<unsigned long long>(state.stats.lifeMismatch),
                     static_cast<unsigned long long>(state.stats.maxMinorAge), state.current.size(),
                     state.previous.size(), static_cast<unsigned long long>(state.stats.peak), state.capacity,
                     static_cast<unsigned>(ProductEnabled()));
        std::fprintf(stderr,
                     "[REMAPDOMAIN][distribution] major=%llu type0=%llu type1=%llu type2=%llu type3=%llu "
                     "type4=%llu type5=%llu type6=%llu type7=%llu type8=%llu type9=%llu type10=%llu "
                     "type11=%llu type12=%llu type13=%llu type14=%llu type15=%llu\n",
                     static_cast<unsigned long long>(state.currentMajor),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[0]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[1]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[2]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[3]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[4]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[5]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[6]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[7]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[8]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[9]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[10]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[11]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[12]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[13]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[14]),
                     static_cast<unsigned long long>(state.roundTrackedByHolderType[15]));
        std::fflush(stderr);
    }
    state.oracleFixed.clear();
    state.postflipOpen = false;
}

Snapshot GetSnapshot()
{
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    Snapshot snapshot = state.stats;
    snapshot.current = state.current.size();
    snapshot.previous = state.previous.size();
    snapshot.awaitingOracle = state.awaitingOracle.size();
    snapshot.candidates = state.oldMarkCandidates.size() + state.promotedCandidates.size() +
        state.youngHolderRegions.size();
    snapshot.capacity = state.capacity;
    return snapshot;
}

void Report(const char* point)
{
    if (!Active()) {
        return;
    }
    Snapshot snapshot = GetSnapshot();
    std::fprintf(stderr,
                 "[REMAPDOMAIN][summary] point=%s audit=%u product=%u staged=%llu captured=%llu mark=%llu move=%llu "
                 "barrier=%llu retainFailed=%llu duplicate=%llu consumed=%llu simulated=%llu requeue=%llu "
                 "lifeMismatch=%llu holderDead=%llu missing=%llu extra=%llu oracleTracked=%llu "
                 "oracleFixed=%llu postflipObserved=%llu postflipObservedFixed=%llu flips=%llu "
                 "maxMinorAge=%llu current=%llu previous=%llu awaiting=%llu "
                 "candidates=%llu peak=%llu capacity=%llu postflipRounds=%llu\n",
                 point == nullptr ? "?" : point, static_cast<unsigned>(AuditEnabled()),
                 static_cast<unsigned>(ProductEnabled()), static_cast<unsigned long long>(snapshot.staged),
                 static_cast<unsigned long long>(snapshot.captured),
                 static_cast<unsigned long long>(snapshot.capturedByProducer[0]),
                 static_cast<unsigned long long>(snapshot.capturedByProducer[1]),
                 static_cast<unsigned long long>(snapshot.capturedByProducer[2]),
                 static_cast<unsigned long long>(snapshot.retainFailed),
                 static_cast<unsigned long long>(snapshot.duplicate),
                 static_cast<unsigned long long>(snapshot.consumed),
                 static_cast<unsigned long long>(snapshot.simulatedHeal),
                 static_cast<unsigned long long>(snapshot.requeue),
                 static_cast<unsigned long long>(snapshot.lifeMismatch),
                 static_cast<unsigned long long>(snapshot.holderDead),
                 static_cast<unsigned long long>(snapshot.missing),
                 static_cast<unsigned long long>(snapshot.extra),
                 static_cast<unsigned long long>(snapshot.oracleTracked),
                 static_cast<unsigned long long>(snapshot.oracleFixed),
                 static_cast<unsigned long long>(snapshot.postflipObserved),
                 static_cast<unsigned long long>(snapshot.postflipObservedFixed),
                 static_cast<unsigned long long>(snapshot.flips),
                 static_cast<unsigned long long>(snapshot.maxMinorAge),
                 static_cast<unsigned long long>(snapshot.current),
                 static_cast<unsigned long long>(snapshot.previous),
                 static_cast<unsigned long long>(snapshot.awaitingOracle),
                 static_cast<unsigned long long>(snapshot.candidates),
                 static_cast<unsigned long long>(snapshot.peak),
                 static_cast<unsigned long long>(snapshot.capacity),
                 static_cast<unsigned long long>(snapshot.postflipRounds));
    std::fflush(stderr);
}

void ResetForTesting(size_t capacity)
{
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    state.current.clear();
    state.previous.clear();
    state.awaitingOracle.clear();
    state.oracleTrackedSlots.clear();
    state.oracleTargets.clear();
    state.publishOutcome.clear();
    state.oldMarkCandidates.clear();
    state.promotedCandidates.clear();
    state.youngHolderRegions.clear();
    state.oracleFixed.clear();
    state.stats = Snapshot{};
    state.capacity = capacity == 0 ? 1 : capacity;
    state.currentMinor = 0;
    state.currentMajor = 0;
    state.roundTracked = 0;
    state.roundFixed = 0;
    state.roundObserved = 0;
    state.roundObservedFixed = 0;
    std::memset(state.roundTrackedByHolderType, 0, sizeof(state.roundTrackedByHolderType));
    state.postflipOpen = false;
    state.testing = true;
}

bool InsertForTesting(const Record& input)
{
    State& state = Domain();
    std::lock_guard<std::mutex> guard(state.mutex);
    Record record = input;
    record.birthMinor = record.birthMinor == 0 ? state.currentMinor : record.birthMinor;
    return InsertCurrentUnlocked(state, record, true);
}

} // namespace DeferredRemapDomain
} // namespace MapleRuntime
