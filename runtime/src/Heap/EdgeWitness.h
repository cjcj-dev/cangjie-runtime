// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EDGE_WITNESS_H
#define MRT_EDGE_WITNESS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class BaseObject;

// Read-only three-point edge witness:
//   P1 consume  = RescanRememberedSet, before GetAndTryTagObj
//   P2 register = FixEdgeSet::MaybeAdd (after Add)
//   P3 rewrite  = FixHolderForwardRefField (after successful CAS)
//
// Logical key = (holder, slot, majorEpoch). Practically:
//   - P2/P3 flag table is slot-keyed (FixEdgeSet itself keys by slot; BulkForward
//     VisitAndClear has no holder).
//   - P1 samples record holder+slot+epoch and look up slot flags.
// ⛔ Never filters on IsSurvivedObject (survived is recorded only).
// ⛔ Never dereferences target; never calls IsValidObject / GetAndTryTagObj /
// FindLatestVersion / FindToVersion. Holder validity = bare header non-zero only.
class EdgeWitness {
public:
    static EdgeWitness& Instance() noexcept;

    void OnConsume(BaseObject* holder, RefField<>* slot);
    void OnRegistered(BaseObject* holder, RefField<>* slot);
    void OnRewritten(BaseObject* holder, RefField<>* slot);
    void DumpAndReset(const char* where);

    size_t OverflowCount() const { return overflow.load(std::memory_order_relaxed); }

private:
    EdgeWitness() = default;
    ~EdgeWitness() = default;
    EdgeWitness(const EdgeWitness&) = delete;
    EdgeWitness& operator=(const EdgeWitness&) = delete;

    static constexpr size_t SAMPLE_CAP = 4096;
    static constexpr size_t FLAG_CAP = 16384;

    struct Sample {
        BaseObject* holder{ nullptr };
        RefField<>* slot{ nullptr };
        size_t majorEpoch{ 0 };
        uint8_t plain{ 0 };
        uint8_t registered{ 0 }; // snapshot of slot flag at P1
        uint8_t rewritten{ 0 };
        uint8_t holderSurvived{ 0 }; // record only
        uint8_t holderRegionType{ 0xFF };
        uint8_t retainedState{ 0xFF };
        uint8_t targetFrom{ 0 };
        uint8_t targetGhost{ 0 };
        uint8_t targetYoung{ 0 };
        uint8_t used{ 0 };
    };

    struct SlotFlag {
        RefField<>* slot{ nullptr };
        uint8_t registered{ 0 };
        uint8_t rewritten{ 0 };
        uint8_t used{ 0 };
    };

    SlotFlag* FindFlag(RefField<>* slot);
    SlotFlag* FindOrInsertFlag(RefField<>* slot, bool* overflowed);
    static size_t CurrentEpoch();

    std::mutex mutex;
    Sample samples[SAMPLE_CAP];
    size_t sampleCount{ 0 };
    SlotFlag flags[FLAG_CAP];
    size_t flagCount{ 0 };

    std::atomic<size_t> overflow{ 0 };
    std::atomic<size_t> p1Hits{ 0 };
    std::atomic<size_t> p1Plain{ 0 };
    std::atomic<size_t> unregUnrew{ 0 };
    std::atomic<size_t> regCount{ 0 };
    std::atomic<size_t> rewCount{ 0 };

    // Positive-control fixture: first registered slot must later rewrite.
    RefField<>* fixtureSlot{ nullptr };
    BaseObject* fixtureHolder{ nullptr };
    size_t fixtureEpoch{ 0 };
    uint8_t fixtureRegistered{ 0 };
    uint8_t fixtureRewritten{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_EDGE_WITNESS_H
