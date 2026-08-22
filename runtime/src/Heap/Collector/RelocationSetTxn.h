// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_RELOCATION_SET_TXN_H
#define MRT_RELOCATION_SET_TXN_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Common/TypeDef.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Collector/RegionLifeClock.h"

namespace MapleRuntime {
class RegionInfo;
class ZForwarding;

// Set-level visibility and retirement boundary modelled after
// ZRelocationSet::install/reset (zRelocationSet.cpp:91-96,106-133,170-176,191-203).
class RelocationSetTxn final {
public:
    enum class State : uint8_t {
        BUILDING = 0,
        PUBLISHED = 1,
        COPY_CLOSED = 2,
        REMAP_CLOSED = 3,
        DETACHED = 4,
        DESTROYED = 5,
    };

    enum class PlanSlot : uint8_t {
        UNPLANNED = 0,
        PLANNING = 1,
        PLANNED = 2,
        IN_PLACE = 3,
        EXEMPT = 4,
    };

    struct Participant {
        RegionLifeId fromLife = 0;
        RegionInfo* region = nullptr;
        MAddress start = 0;
        size_t size = 0;
        ZForwarding* forwardingEnvelope = nullptr;
        std::shared_ptr<std::atomic<uint8_t>> planSlot;
    };

    struct Counters {
        uint64_t prepareCalls;
        uint64_t duplicatePrepare;
        uint64_t prepareIntersection;
        uint64_t entriesReload;
        uint64_t partialInstallWindows;
        uint64_t published;
        uint64_t rollback;
        uint64_t participantLifeMismatch;
        uint64_t handlesAcquired;
        uint64_t handleLeaks;
        uint64_t handlesPeak;
        uint64_t outstandingPeak;
        uint64_t retiredPeak;
        uint64_t destroyed;
        uint64_t finishIncomplete;
        uint64_t duplicateInstall;
    };

    class Handle {
    public:
        Handle() = default;
        Handle(Handle&& other) noexcept;
        Handle& operator=(Handle&& other) noexcept;
        ~Handle();
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        explicit operator bool() const { return txn != nullptr && participant != nullptr; }
        const Participant* GetParticipant() const { return participant; }
        RegionInfo* GetRegion() const { return participant == nullptr ? nullptr : participant->region; }
        ZForwarding* GetEnvelope() const
        {
            return participant == nullptr ? nullptr : participant->forwardingEnvelope;
        }
        uint64_t Id() const;
        State GetState() const;
        void AddOutstanding(uint64_t count = 1);
        void AckOutstanding(uint64_t count = 1);

    private:
        Handle(RelocationSetTxn* txnIn, const Participant* participantIn);
        void Reset();

        RelocationSetTxn* txn = nullptr;
        const Participant* participant = nullptr;
        friend class RelocationSetTxn;
    };

    class Builder {
    public:
        explicit Builder(Generation generation);
        ~Builder();
        Builder(Builder&&) noexcept = default;
        Builder& operator=(Builder&&) noexcept = default;
        Builder(const Builder&) = delete;
        Builder& operator=(const Builder&) = delete;

        bool AddParticipant(RegionInfo* region);
        bool AddParticipantForTest(RegionInfo* region, MAddress start, size_t size, RegionLifeId life,
                                   ZForwarding* envelope = nullptr);
        bool AttachEnvelope(RegionInfo* region, ZForwarding* envelope);
        bool BuildSucceeded() const { return buildSucceeded; }
        size_t Size() const;
        bool TryPublish();
        void PublishOrFatal();
        void Rollback();

    private:
        std::shared_ptr<RelocationSetTxn> txn;
        bool buildSucceeded = true;
        bool completed = false;
        size_t attempted = 0;
    };

    static bool Enabled();
    static bool AuditEnabled();
    static Handle AcquireForAddress(MAddress address);
    static Handle AcquireParticipant(RegionInfo* region, RegionLifeId life);
    static void NotePlan(RegionInfo* region, PlanSlot slot);
    static void NoteFinishIncomplete(Generation generation);
    static void CloseCopy(Generation generation);
    static void CloseRemap(Generation generation);
    static void OnForwardingReaderExit();

    // Proof leg consumed by the existing FromPageDetachCheck. No second reuse gate.
    static bool HasDetachEvidence(const RegionInfo* region, RegionLifeId life,
                                  uint64_t* handles = nullptr, uint64_t* outstanding = nullptr);

    static void AddOutstanding(Generation generation, uint64_t count = 1);
    static void AckOutstanding(Generation generation, uint64_t count = 1);
    static State ActiveStateForTest(Generation generation);
    static size_t ActiveParticipantsForTest(Generation generation);
    static Counters GetCounters();
    static void DumpSummary();

private:
    explicit RelocationSetTxn(Generation generation);

public:
    ~RelocationSetTxn();

private:
    static size_t GenerationIndex(Generation generation);
    static void Publish(const std::shared_ptr<RelocationSetTxn>& txn);
    static RelocationSetTxn* PinActive(size_t index, bool countStats);
    static void UnpinActive(RelocationSetTxn* txn, bool countStats);
    static void ReleaseHandle(RelocationSetTxn* txn);
    static void CollectRetired();
    bool TryDetach();
    void FreezeProductTables();
    static const Participant* FindParticipant(const RelocationSetTxn& txn, MAddress address,
                                              RegionInfo* exactRegion = nullptr);

    uint64_t id;
    Generation generation;
    std::atomic<State> state{ State::BUILDING };
    std::atomic<uint64_t> externalHandles{ 0 };
    std::atomic<uint64_t> remapOutstanding{ 0 };
    std::vector<Participant> participants;
    MAddress participantIndexBase = 0;
    size_t participantIndexGranule = 0;
    std::vector<uint32_t> participantIndex;
};

} // namespace MapleRuntime

#endif // MRT_RELOCATION_SET_TXN_H
