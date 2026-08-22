// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/RelocationSetTxn.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sched.h>

#include "Base/Log.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Verify/FromPageDetachCheck.h"

namespace MapleRuntime {
namespace {
constexpr size_t kGenerationCount = 2;
constexpr size_t kMaxParticipants = 65536;
constexpr uint64_t kMaxHandles = 1U << 20;
constexpr uint64_t kMaxOutstanding = 1U << 24;
constexpr size_t kMaxRetiredTxns = 65536;
constexpr size_t kMaxParticipantIndexSlots = 1U << 24;
constexpr uint32_t kNoParticipant = UINT32_MAX;

std::atomic<RelocationSetTxn*> g_activeRelocationTxn[kGenerationCount];
std::shared_ptr<RelocationSetTxn> g_activeRelocationOwner[kGenerationCount];
std::atomic<uint64_t> g_handleAdmissions[kGenerationCount];
std::mutex g_retiredMutex;
std::vector<std::shared_ptr<RelocationSetTxn>> g_retiredTxns;
std::atomic<uint64_t> g_nextTxnId{ 1 };
thread_local const RelocationSetTxn* g_detachPermitTxn = nullptr;
thread_local RegionLifeId g_detachPermitLife = 0;

class DetachPermitScope {
public:
    DetachPermitScope(const RelocationSetTxn* txn, RegionLifeId life)
        : previousTxn(g_detachPermitTxn), previousLife(g_detachPermitLife)
    {
        g_detachPermitTxn = txn;
        g_detachPermitLife = life;
    }

    ~DetachPermitScope()
    {
        g_detachPermitTxn = previousTxn;
        g_detachPermitLife = previousLife;
    }

private:
    const RelocationSetTxn* previousTxn;
    RegionLifeId previousLife;
};

struct AtomicCounters {
    std::atomic<uint64_t> prepareCalls{ 0 };
    std::atomic<uint64_t> duplicatePrepare{ 0 };
    std::atomic<uint64_t> prepareIntersection{ 0 };
    std::atomic<uint64_t> entriesReload{ 0 };
    std::atomic<uint64_t> partialInstallWindows{ 0 };
    std::atomic<uint64_t> published{ 0 };
    std::atomic<uint64_t> rollback{ 0 };
    std::atomic<uint64_t> participantLifeMismatch{ 0 };
    std::atomic<uint64_t> handlesAcquired{ 0 };
    std::atomic<uint64_t> liveHandles{ 0 };
    std::atomic<uint64_t> handlesPeak{ 0 };
    std::atomic<uint64_t> outstandingPeak{ 0 };
    std::atomic<uint64_t> retiredPeak{ 0 };
    std::atomic<uint64_t> destroyed{ 0 };
    std::atomic<uint64_t> finishIncomplete{ 0 };
    std::atomic<uint64_t> duplicateInstall{ 0 };
};
AtomicCounters g_counters;

void NotePeak(std::atomic<uint64_t>& peak, uint64_t value)
{
    uint64_t old = peak.load(std::memory_order_relaxed);
    while (value > old && !peak.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
}

void WaitForHandleAdmissions(size_t index, const char* site)
{
    while (g_handleAdmissions[index].load(std::memory_order_acquire) != 0) {
        sched_yield();
    }
    (void)site;
}

bool EnvOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

long FailureParticipant()
{
    const char* value = std::getenv("CJRT_RELOC_TXN_FAIL_PARTICIPANT");
    if (value == nullptr || *value == '\0') {
        return -1;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value && *end == '\0' && parsed > 0 ? parsed : -1;
}

struct DumpAtExit {
    DumpAtExit() { std::atexit(RelocationSetTxn::DumpSummary); }
};
const DumpAtExit g_dumpAtExit;
} // namespace

size_t RelocationSetTxn::GenerationIndex(Generation generation)
{
    return generation == Generation::Young ? 0 : 1;
}

bool RelocationSetTxn::Enabled()
{
    static const bool enabled = EnvOne("CJRT_RELOC_TXN");
    return enabled;
}

bool RelocationSetTxn::AuditEnabled()
{
    static const bool enabled = EnvOne("RELOCTXN_AUDIT");
    return enabled;
}

RelocationSetTxn::RelocationSetTxn(Generation generationIn)
    : id(g_nextTxnId.fetch_add(1, std::memory_order_relaxed)), generation(generationIn)
{
}

RelocationSetTxn::~RelocationSetTxn()
{
    CHECK(externalHandles.load(std::memory_order_acquire) == 0);
    CHECK(remapOutstanding.load(std::memory_order_acquire) == 0);
    if (Enabled() && state.load(std::memory_order_acquire) != State::BUILDING) {
        CHECK(state.load(std::memory_order_acquire) == State::DETACHED);
    }
    state.store(State::DESTROYED, std::memory_order_release);
    g_counters.destroyed.fetch_add(1, std::memory_order_relaxed);
}

void RelocationSetTxn::FreezeProductTables()
{
    if (!Enabled()) {
        return;
    }
    for (const Participant& p : participants) {
        if (p.region != nullptr) {
            ForwardingTable::RetireEnvelope(p.forwardingEnvelope);
        }
    }
}

bool RelocationSetTxn::TryDetach()
{
    if (!Enabled()) {
        state.store(State::DETACHED, std::memory_order_release);
        return true;
    }
    bool detached = true;
    for (const Participant& p : participants) {
        if (p.region == nullptr) {
            continue;
        }
        if (p.region->GetRegionLifeId() != p.fromLife) {
            g_counters.participantLifeMismatch.fetch_add(1, std::memory_order_relaxed);
        }
        // This is the existing detach checkpoint's permit for exactly this
        // transaction/life. The registry remains reuse evidence for every
        // other caller until all participants have passed the checkpoint.
        DetachPermitScope permit(this, p.fromLife);
        (void)FromPageDetach::FromPageDetachCheck(p.region, FromPageDetach::Site::MAJOR_RECHECK,
                                                  FromPageDetach::Action::MAJOR_CLOSE);
        // zGeneration.cpp:276-284 reset_relocation_set removes forwarding after
        // relocate; this is that drop. take_garbage must not have to wait for it.
        ForwardingTable::DropRetiredCovering(p.start, p.size, true);
        detached = detached && !ForwardingTable::RetiredCovers(p.start, p.size);
    }
    if (detached) {
        state.store(State::DETACHED, std::memory_order_release);
    }
    return detached;
}

RelocationSetTxn::Builder::Builder(Generation generation)
    : txn(std::shared_ptr<RelocationSetTxn>(new RelocationSetTxn(generation)))
{
    g_counters.prepareCalls.fetch_add(1, std::memory_order_relaxed);
}

RelocationSetTxn::Builder::~Builder()
{
    if (!completed && txn != nullptr) {
        Rollback();
    }
}

bool RelocationSetTxn::Builder::AddParticipantForTest(RegionInfo* region, MAddress start, size_t size,
                                                       RegionLifeId life, ZForwarding* envelope)
{
    ++attempted;
    if (attempted > kMaxParticipants) {
        LOG(RTLOG_FATAL, "CJRT_RELOC_TXN participant overflow txn=%llu participants=%zu max=%zu",
            static_cast<unsigned long long>(txn->id), attempted, kMaxParticipants);
    }
    if (FailureParticipant() == static_cast<long>(attempted)) {
        buildSucceeded = false;
        return false;
    }
    for (const Participant& p : txn->participants) {
        if ((region != nullptr && p.region == region) || (start != 0 && p.start == start)) {
            buildSucceeded = false;
            return false;
        }
    }
    Participant p;
    p.region = region;
    p.start = start;
    p.size = size;
    p.fromLife = life;
    p.forwardingEnvelope = envelope;
    p.planSlot = std::make_shared<std::atomic<uint8_t>>(static_cast<uint8_t>(PlanSlot::UNPLANNED));
    txn->participants.push_back(std::move(p));
    return true;
}

bool RelocationSetTxn::Builder::AddParticipant(RegionInfo* region)
{
    if (region == nullptr) {
        buildSucceeded = false;
        return false;
    }
    return AddParticipantForTest(region, region->GetRegionStart(), region->GetRegionSize(),
                                 region->GetRegionLifeId(), ForwardingTable::GetEntries(region->GetRegionStart()));
}

bool RelocationSetTxn::Builder::AttachEnvelope(RegionInfo* region, ZForwarding* envelope)
{
    if (completed || region == nullptr || envelope == nullptr) {
        buildSucceeded = false;
        return false;
    }
    for (Participant& p : txn->participants) {
        if (p.region == region) {
            if (p.fromLife != region->GetRegionLifeId()) {
                g_counters.participantLifeMismatch.fetch_add(1, std::memory_order_relaxed);
                buildSucceeded = false;
                return false;
            }
            if (p.forwardingEnvelope != nullptr) {
                g_counters.entriesReload.fetch_add(1, std::memory_order_relaxed);
            }
            p.forwardingEnvelope = envelope;
            g_counters.partialInstallWindows.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    buildSucceeded = false;
    return false;
}

size_t RelocationSetTxn::Builder::Size() const
{
    return txn == nullptr ? 0 : txn->participants.size();
}

bool RelocationSetTxn::Builder::TryPublish()
{
    if (completed || txn == nullptr) {
        g_counters.duplicateInstall.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!buildSucceeded) {
        Rollback();
        return false;
    }
    for (const Participant& p : txn->participants) {
        if (p.region != nullptr && p.forwardingEnvelope == nullptr) {
            buildSucceeded = false;
            Rollback();
            return false;
        }
    }
    // Publication freezes membership, so build the reader index before the
    // release linearization point. Forwarding/barrier lookups are hot enough
    // that a per-read CSet walk is not an acceptable handle-first protocol.
    std::sort(txn->participants.begin(), txn->participants.end(),
              [](const Participant& left, const Participant& right) {
                  return left.start < right.start;
              });
    if (!txn->participants.empty()) {
        txn->participantIndexGranule = RegionInfo::UNIT_SIZE;
        CHECK_DETAIL(txn->participantIndexGranule != 0,
                     "CJRT_RELOC_TXN index granule is zero");
        const size_t granule = txn->participantIndexGranule;
        txn->participantIndexBase = txn->participants.front().start -
            txn->participants.front().start % granule;
        MAddress end = txn->participantIndexBase;
        for (const Participant& p : txn->participants) {
            CHECK_DETAIL(p.size <= UINTPTR_MAX - p.start,
                         "CJRT_RELOC_TXN participant range overflow start=%#zx size=%zu",
                         p.start, p.size);
            end = std::max(end, p.start + p.size);
        }
        const size_t span = end - txn->participantIndexBase;
        const size_t slots = span / granule + ((span % granule) != 0 ? 1 : 0);
        CHECK_DETAIL(slots <= kMaxParticipantIndexSlots,
                     "CJRT_RELOC_TXN index overflow slots=%zu max=%zu",
                     slots, kMaxParticipantIndexSlots);
        txn->participantIndex.assign(slots, kNoParticipant);
        for (size_t i = 0; i < txn->participants.size(); ++i) {
            const Participant& p = txn->participants[i];
            if (p.size == 0) {
                continue;
            }
            const size_t first = (p.start - txn->participantIndexBase) / granule;
            const size_t endOffset = p.start + p.size - txn->participantIndexBase;
            const size_t last = endOffset / granule + ((endOffset % granule) != 0 ? 1 : 0);
            CHECK_DETAIL(i <= UINT32_MAX, "CJRT_RELOC_TXN participant index overflow index=%zu", i);
            for (size_t slot = first; slot < last; ++slot) {
                CHECK_DETAIL(slot < txn->participantIndex.size() &&
                                 txn->participantIndex[slot] == kNoParticipant,
                             "CJRT_RELOC_TXN overlapping participant index slot=%zu", slot);
                txn->participantIndex[slot] = static_cast<uint32_t>(i);
            }
        }
    }
    RelocationSetTxn::Publish(txn);
    completed = true;
    txn.reset();
    return true;
}

void RelocationSetTxn::Builder::PublishOrFatal()
{
    CHECK_DETAIL(TryPublish(), "CJRT_RELOC_TXN install failure: duplicate or incomplete transaction");
}

void RelocationSetTxn::Builder::Rollback()
{
    if (completed) {
        return;
    }
    completed = true;
    g_counters.rollback.fetch_add(1, std::memory_order_relaxed);
    txn.reset();
}

const RelocationSetTxn::Participant* RelocationSetTxn::FindParticipant(const RelocationSetTxn& txn,
                                                                       MAddress address,
                                                                       RegionInfo* exactRegion)
{
    if (txn.participantIndexGranule == 0 || address < txn.participantIndexBase) {
        return nullptr;
    }
    const size_t slot = (address - txn.participantIndexBase) / txn.participantIndexGranule;
    if (slot >= txn.participantIndex.size()) {
        return nullptr;
    }
    const uint32_t index = txn.participantIndex[slot];
    if (index == kNoParticipant || index >= txn.participants.size()) {
        return nullptr;
    }
    const Participant& p = txn.participants[index];
    if (exactRegion != nullptr) {
        return p.region == exactRegion ? &p : nullptr;
    }
    return p.size != 0 && address >= p.start && address - p.start < p.size ? &p : nullptr;
}

void RelocationSetTxn::Publish(const std::shared_ptr<RelocationSetTxn>& txn)
{
    CHECK(txn != nullptr);
    CHECK(txn->state.load(std::memory_order_acquire) == State::BUILDING);
    const size_t index = GenerationIndex(txn->generation);
    std::shared_ptr<RelocationSetTxn> previous = std::move(g_activeRelocationOwner[index]);
    size_t intersection = 0;
    if (previous != nullptr) {
        for (const Participant& p : txn->participants) {
            const Participant* old = FindParticipant(*previous, p.start);
            if (old != nullptr && old->fromLife == p.fromLife) {
                ++intersection;
            }
        }
    }
    if (intersection != 0) {
        g_counters.duplicatePrepare.fetch_add(1, std::memory_order_relaxed);
        g_counters.prepareIntersection.fetch_add(intersection, std::memory_order_relaxed);
    }

    txn->state.store(State::PUBLISHED, std::memory_order_relaxed);
    // The sole set visibility linearization point. All participant/envelope writes happen-before it.
    g_activeRelocationOwner[index] = txn;
    RelocationSetTxn* previousRaw =
        g_activeRelocationTxn[index].exchange(txn.get(), std::memory_order_release);
    CHECK_DETAIL(previous.get() == previousRaw,
                 "CJRT_RELOC_TXN active owner mismatch publish owner=%p active=%p",
                 previous.get(), previousRaw);
    WaitForHandleAdmissions(index, "publish");
    g_counters.published.fetch_add(1, std::memory_order_relaxed);
    if (previous != nullptr) {
        previous->state.store(State::COPY_CLOSED, std::memory_order_release);
        previous->FreezeProductTables();
        if (previous->remapOutstanding.load(std::memory_order_acquire) == 0) {
            previous->state.store(State::REMAP_CLOSED, std::memory_order_release);
        }
        std::lock_guard<std::mutex> lock(g_retiredMutex);
        if (g_retiredTxns.size() >= kMaxRetiredTxns) {
            LOG(RTLOG_FATAL, "CJRT_RELOC_TXN retired overflow entries=%zu max=%zu",
                g_retiredTxns.size(), kMaxRetiredTxns);
        }
        g_retiredTxns.push_back(std::move(previous));
        NotePeak(g_counters.retiredPeak, g_retiredTxns.size());
    }
    CollectRetired();
}

RelocationSetTxn::Handle::Handle(RelocationSetTxn* txnIn, const Participant* participantIn)
    : txn(txnIn), participant(participantIn)
{
}

RelocationSetTxn::Handle::Handle(Handle&& other) noexcept
    : txn(other.txn), participant(other.participant)
{
    other.txn = nullptr;
    other.participant = nullptr;
}

RelocationSetTxn::Handle& RelocationSetTxn::Handle::operator=(Handle&& other) noexcept
{
    if (this != &other) {
        Reset();
        txn = other.txn;
        participant = other.participant;
        other.txn = nullptr;
        other.participant = nullptr;
    }
    return *this;
}

RelocationSetTxn::Handle::~Handle() { Reset(); }

void RelocationSetTxn::Handle::Reset()
{
    if (txn != nullptr) {
        RelocationSetTxn* raw = txn;
        txn = nullptr;
        participant = nullptr;
        ReleaseHandle(raw);
    }
}

uint64_t RelocationSetTxn::Handle::Id() const { return txn == nullptr ? 0 : txn->id; }

RelocationSetTxn::State RelocationSetTxn::Handle::GetState() const
{
    return txn == nullptr ? State::DESTROYED : txn->state.load(std::memory_order_acquire);
}

void RelocationSetTxn::Handle::AddOutstanding(uint64_t count)
{
    CHECK(txn != nullptr);
    const uint64_t now = txn->remapOutstanding.fetch_add(count, std::memory_order_acq_rel) + count;
    if (now > kMaxOutstanding) {
        LOG(RTLOG_FATAL, "CJRT_RELOC_TXN outstanding overflow txn=%llu outstanding=%llu max=%llu",
            static_cast<unsigned long long>(txn->id), static_cast<unsigned long long>(now),
            static_cast<unsigned long long>(kMaxOutstanding));
    }
    NotePeak(g_counters.outstandingPeak, now);
}

void RelocationSetTxn::Handle::AckOutstanding(uint64_t count)
{
    CHECK(txn != nullptr);
    const uint64_t old = txn->remapOutstanding.fetch_sub(count, std::memory_order_acq_rel);
    CHECK_DETAIL(old >= count, "CJRT_RELOC_TXN outstanding underflow txn=%llu old=%llu ack=%llu",
                 static_cast<unsigned long long>(txn->id), static_cast<unsigned long long>(old),
                 static_cast<unsigned long long>(count));
    if (old == count && txn->state.load(std::memory_order_acquire) == State::COPY_CLOSED) {
        txn->state.store(State::REMAP_CLOSED, std::memory_order_release);
    }
    CollectRetired();
}

RelocationSetTxn::Handle RelocationSetTxn::AcquireForAddress(MAddress address)
{
    for (size_t i = 0; i < kGenerationCount; ++i) {
        RelocationSetTxn* txn = PinActive(i, true);
        if (txn == nullptr) {
            continue;
        }
        const Participant* participant = FindParticipant(*txn, address);
        if (participant == nullptr) {
            ReleaseHandle(txn);
            continue;
        }
        return Handle(txn, participant);
    }
    return Handle();
}

RelocationSetTxn::Handle RelocationSetTxn::AcquireParticipant(RegionInfo* region, RegionLifeId life)
{
    if (region == nullptr) {
        return Handle();
    }
    Handle handle = AcquireForAddress(region->GetRegionStart());
    if (!handle) {
        return handle;
    }
    if (handle.participant->region != region || handle.participant->fromLife != life ||
        region->GetRegionLifeId() != life) {
        g_counters.participantLifeMismatch.fetch_add(1, std::memory_order_relaxed);
        handle.Reset();
    }
    return handle;
}

RelocationSetTxn* RelocationSetTxn::PinActive(size_t index, bool countStats)
{
    const uint64_t admissions =
        g_handleAdmissions[index].fetch_add(1, std::memory_order_acq_rel) + 1;
    if (admissions > kMaxHandles) {
        LOG(RTLOG_FATAL, "CJRT_RELOC_TXN admission overflow generation=%zu admissions=%llu max=%llu",
            index, static_cast<unsigned long long>(admissions),
            static_cast<unsigned long long>(kMaxHandles));
    }
    RelocationSetTxn* txn = g_activeRelocationTxn[index].load(std::memory_order_acquire);
    if (txn != nullptr) {
        const uint64_t handles = txn->externalHandles.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (handles > kMaxHandles) {
            LOG(RTLOG_FATAL, "CJRT_RELOC_TXN handle overflow txn=%llu handles=%llu max=%llu",
                static_cast<unsigned long long>(txn->id), static_cast<unsigned long long>(handles),
                static_cast<unsigned long long>(kMaxHandles));
        }
    }
    g_handleAdmissions[index].fetch_sub(1, std::memory_order_release);
    if (txn == nullptr) {
        return nullptr;
    }
    if (g_activeRelocationTxn[index].load(std::memory_order_acquire) != txn ||
        txn->state.load(std::memory_order_acquire) != State::PUBLISHED) {
        UnpinActive(txn, false);
        return nullptr;
    }
    if (countStats) {
        g_counters.handlesAcquired.fetch_add(1, std::memory_order_relaxed);
        const uint64_t live = g_counters.liveHandles.fetch_add(1, std::memory_order_relaxed) + 1;
        NotePeak(g_counters.handlesPeak, live);
    }
    return txn;
}

void RelocationSetTxn::UnpinActive(RelocationSetTxn* txn, bool countStats)
{
    CHECK(txn != nullptr);
    const uint64_t old = txn->externalHandles.fetch_sub(1, std::memory_order_acq_rel);
    CHECK(old > 0);
    if (countStats) {
        g_counters.liveHandles.fetch_sub(1, std::memory_order_relaxed);
    }
    if (old == 1 && txn->state.load(std::memory_order_acquire) >= State::COPY_CLOSED) {
        CollectRetired();
    }
}

void RelocationSetTxn::ReleaseHandle(RelocationSetTxn* txn)
{
    UnpinActive(txn, true);
}

void RelocationSetTxn::CollectRetired()
{
    std::vector<std::shared_ptr<RelocationSetTxn>> candidates;
    {
        std::lock_guard<std::mutex> lock(g_retiredMutex);
        for (const auto& entry : g_retiredTxns) {
            RelocationSetTxn* txn = entry.get();
            const bool remapClosed = txn->state.load(std::memory_order_acquire) >= State::REMAP_CLOSED;
            if (entry.use_count() == 1 &&
                txn->externalHandles.load(std::memory_order_acquire) == 0 && remapClosed &&
                txn->remapOutstanding.load(std::memory_order_acquire) == 0) {
                candidates.push_back(entry);
            }
        }
    }
    // The checkpoint consults the registry, so run it without the registry lock.
    for (const auto& txn : candidates) {
        (void)txn->TryDetach();
    }
    std::vector<std::shared_ptr<RelocationSetTxn>> destroy;
    {
        std::lock_guard<std::mutex> lock(g_retiredMutex);
        auto it = g_retiredTxns.begin();
        while (it != g_retiredTxns.end()) {
            if ((*it)->state.load(std::memory_order_acquire) == State::DETACHED) {
                destroy.push_back(std::move(*it));
                it = g_retiredTxns.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Destructors run after the registry no longer names these transactions.
    destroy.clear();
}

void RelocationSetTxn::NotePlan(RegionInfo* region, PlanSlot slot)
{
    if (region == nullptr) {
        return;
    }
    Handle handle = AcquireParticipant(region, region->GetRegionLifeId());
    if (!handle) {
        return;
    }
    handle.participant->planSlot->store(static_cast<uint8_t>(slot), std::memory_order_release);
}

void RelocationSetTxn::NoteFinishIncomplete(Generation)
{
    g_counters.finishIncomplete.fetch_add(1, std::memory_order_relaxed);
}

void RelocationSetTxn::CloseCopy(Generation generation)
{
    if (!Enabled()) {
        return;
    }
    const size_t index = GenerationIndex(generation);
    std::shared_ptr<RelocationSetTxn> txn = std::move(g_activeRelocationOwner[index]);
    RelocationSetTxn* raw =
        g_activeRelocationTxn[index].exchange(nullptr, std::memory_order_acq_rel);
    CHECK_DETAIL(txn.get() == raw,
                 "CJRT_RELOC_TXN active owner mismatch close owner=%p active=%p",
                 txn.get(), raw);
    WaitForHandleAdmissions(index, "close");
    if (txn == nullptr) {
        return;
    }
    State expected = State::PUBLISHED;
    CHECK_DETAIL(txn->state.compare_exchange_strong(expected, State::COPY_CLOSED,
                                                    std::memory_order_acq_rel),
                 "CJRT_RELOC_TXN copy close state txn=%llu state=%u",
                 static_cast<unsigned long long>(txn->id), static_cast<unsigned>(expected));
    txn->FreezeProductTables();
    std::lock_guard<std::mutex> lock(g_retiredMutex);
    if (g_retiredTxns.size() >= kMaxRetiredTxns) {
        LOG(RTLOG_FATAL, "CJRT_RELOC_TXN retired overflow entries=%zu max=%zu",
            g_retiredTxns.size(), kMaxRetiredTxns);
    }
    g_retiredTxns.push_back(std::move(txn));
    NotePeak(g_counters.retiredPeak, g_retiredTxns.size());
}

void RelocationSetTxn::CloseRemap(Generation generation)
{
    if (!Enabled()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_retiredMutex);
        for (const auto& txn : g_retiredTxns) {
            if (txn->generation != generation ||
                txn->state.load(std::memory_order_acquire) != State::COPY_CLOSED) {
                continue;
            }
            CHECK_DETAIL(txn->remapOutstanding.load(std::memory_order_acquire) == 0,
                         "CJRT_RELOC_TXN remap close with outstanding txn=%llu outstanding=%llu",
                         static_cast<unsigned long long>(txn->id),
                         static_cast<unsigned long long>(
                             txn->remapOutstanding.load(std::memory_order_relaxed)));
            txn->state.store(State::REMAP_CLOSED, std::memory_order_release);
        }
    }
    CollectRetired();
}

void RelocationSetTxn::OnForwardingReaderExit()
{
    if (Enabled()) {
        CollectRetired();
    }
}

void NotifyRelocationTxnReaderExit()
{
    RelocationSetTxn::OnForwardingReaderExit();
}

bool RelocationSetTxn::HasDetachEvidence(const RegionInfo* region, RegionLifeId life,
                                         uint64_t* handles, uint64_t* outstanding)
{
    if (!Enabled() || region == nullptr) {
        if (handles != nullptr) {
            *handles = 0;
        }
        if (outstanding != nullptr) {
            *outstanding = 0;
        }
        return false;
    }
    uint64_t handleCount = 0;
    uint64_t outstandingCount = 0;
    auto inspect = [&](RelocationSetTxn* txn) {
        if (txn == nullptr || FindParticipant(*txn, region->GetRegionStart(), const_cast<RegionInfo*>(region)) == nullptr) {
            return;
        }
        const Participant* p = FindParticipant(*txn, region->GetRegionStart(), const_cast<RegionInfo*>(region));
        if (p->fromLife != life) {
            return;
        }
        if (txn == g_detachPermitTxn && p->fromLife == g_detachPermitLife) {
            return;
        }
        handleCount += txn->externalHandles.load(std::memory_order_acquire);
        outstandingCount += txn->remapOutstanding.load(std::memory_order_acquire);
        if (txn->state.load(std::memory_order_acquire) < State::DETACHED) {
            ++outstandingCount;
        }
    };
    for (size_t i = 0; i < kGenerationCount; ++i) {
        RelocationSetTxn* txn = PinActive(i, false);
        inspect(txn);
        if (txn != nullptr) {
            UnpinActive(txn, false);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_retiredMutex);
        for (const auto& txn : g_retiredTxns) {
            inspect(txn.get());
        }
    }
    if (handles != nullptr) {
        *handles = handleCount;
    }
    if (outstanding != nullptr) {
        *outstanding = outstandingCount;
    }
    return handleCount != 0 || outstandingCount != 0;
}

void RelocationSetTxn::AddOutstanding(Generation generation, uint64_t count)
{
    const size_t index = GenerationIndex(generation);
    RelocationSetTxn* txn = PinActive(index, false);
    CHECK_DETAIL(txn != nullptr, "CJRT_RELOC_TXN outstanding without active transaction generation=%zu", index);
    const uint64_t now = txn->remapOutstanding.fetch_add(count, std::memory_order_acq_rel) + count;
    if (now > kMaxOutstanding) {
        LOG(RTLOG_FATAL, "CJRT_RELOC_TXN outstanding overflow txn=%llu outstanding=%llu max=%llu",
            static_cast<unsigned long long>(txn->id), static_cast<unsigned long long>(now),
            static_cast<unsigned long long>(kMaxOutstanding));
    }
    NotePeak(g_counters.outstandingPeak, now);
    UnpinActive(txn, false);
}

void RelocationSetTxn::AckOutstanding(Generation generation, uint64_t count)
{
    const size_t index = GenerationIndex(generation);
    RelocationSetTxn* txn = PinActive(index, false);
    CHECK_DETAIL(txn != nullptr, "CJRT_RELOC_TXN ack without active transaction generation=%zu", index);
    const uint64_t old = txn->remapOutstanding.fetch_sub(count, std::memory_order_acq_rel);
    CHECK_DETAIL(old >= count, "CJRT_RELOC_TXN outstanding underflow txn=%llu old=%llu ack=%llu",
                 static_cast<unsigned long long>(txn->id), static_cast<unsigned long long>(old),
                 static_cast<unsigned long long>(count));
    if (old == count && txn->state.load(std::memory_order_acquire) == State::COPY_CLOSED) {
        txn->state.store(State::REMAP_CLOSED, std::memory_order_release);
    }
    UnpinActive(txn, false);
    CollectRetired();
}

RelocationSetTxn::State RelocationSetTxn::ActiveStateForTest(Generation generation)
{
    RelocationSetTxn* txn = PinActive(GenerationIndex(generation), false);
    const State state = txn == nullptr ? State::DESTROYED : txn->state.load(std::memory_order_acquire);
    if (txn != nullptr) {
        UnpinActive(txn, false);
    }
    return state;
}

size_t RelocationSetTxn::ActiveParticipantsForTest(Generation generation)
{
    RelocationSetTxn* txn = PinActive(GenerationIndex(generation), false);
    const size_t count = txn == nullptr ? 0 : txn->participants.size();
    if (txn != nullptr) {
        UnpinActive(txn, false);
    }
    return count;
}

RelocationSetTxn::Counters RelocationSetTxn::GetCounters()
{
    auto load = [](const std::atomic<uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return Counters{ load(g_counters.prepareCalls), load(g_counters.duplicatePrepare),
                     load(g_counters.prepareIntersection), load(g_counters.entriesReload),
                     load(g_counters.partialInstallWindows), load(g_counters.published),
                     load(g_counters.rollback), load(g_counters.participantLifeMismatch),
                     load(g_counters.handlesAcquired), load(g_counters.liveHandles),
                     load(g_counters.handlesPeak), load(g_counters.outstandingPeak),
                     load(g_counters.retiredPeak), load(g_counters.destroyed),
                     load(g_counters.finishIncomplete), load(g_counters.duplicateInstall) };
}

void RelocationSetTxn::DumpSummary()
{
    if (!AuditEnabled() && !Enabled()) {
        return;
    }
    const Counters c = GetCounters();
    std::fprintf(stderr,
                 "[RELOCTXN] mode=%s prepare=%llu duplicate_prepare=%llu intersection=%llu "
                 "entries_reload=%llu partial_install_windows=%llu published=%llu rollback=%llu "
                 "life_mismatch=%llu handles_acquired=%llu handle_leaks=%llu handles_peak=%llu "
                 "outstanding_peak=%llu retired_peak=%llu destroyed=%llu finish_incomplete=%llu "
                 "duplicate_install=%llu max_participants=%zu max_handles=%llu max_outstanding=%llu\n",
                 Enabled() ? "enforce" : "shadow", static_cast<unsigned long long>(c.prepareCalls),
                 static_cast<unsigned long long>(c.duplicatePrepare),
                 static_cast<unsigned long long>(c.prepareIntersection),
                 static_cast<unsigned long long>(c.entriesReload),
                 static_cast<unsigned long long>(c.partialInstallWindows),
                 static_cast<unsigned long long>(c.published), static_cast<unsigned long long>(c.rollback),
                 static_cast<unsigned long long>(c.participantLifeMismatch),
                 static_cast<unsigned long long>(c.handlesAcquired), static_cast<unsigned long long>(c.handleLeaks),
                 static_cast<unsigned long long>(c.handlesPeak), static_cast<unsigned long long>(c.outstandingPeak),
                 static_cast<unsigned long long>(c.retiredPeak), static_cast<unsigned long long>(c.destroyed),
                 static_cast<unsigned long long>(c.finishIncomplete),
                 static_cast<unsigned long long>(c.duplicateInstall), kMaxParticipants,
                 static_cast<unsigned long long>(kMaxHandles), static_cast<unsigned long long>(kMaxOutstanding));
    std::fflush(stderr);
}

} // namespace MapleRuntime
