// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_PROBE_READ_ROUTE_DIAG_H
#define MRT_PROBE_READ_ROUTE_DIAG_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Common/TypeDef.h"

namespace MapleRuntime {
namespace ProbeReadRouteDiag {
enum class RootKind : uint8_t { None = 0, MajorRaw = 1, MinorRaw = 2 };

struct RootEvidence {
    uint64_t hits { 0 };
    uint32_t gc { 0 };
    MAddress slot { 0 };
    RootKind kind { RootKind::None };
};

struct EdgeStoreEvidence {
    uint64_t writes { 0 };
    MAddress target { 0 };
    MAddress previous { 0 };
    uint8_t barrierPhase { 0 };
    uint8_t slowPath { 0 };
    uint64_t remsetEpoch { 0 };
    uint64_t remsetRecords { 0 };
    uint64_t remsetConsumes { 0 };
    uint64_t remsetClears { 0 };
    MAddress remsetTarget { 0 };
    uint8_t remsetEvent { 0 };
    uint8_t remsetFace { 0 };
};

enum RemsetEvent : uint8_t {
    REMSET_NONE = 0,
    REMSET_MUTATOR_RECORD = 1,
    REMSET_GC_RECORD = 2,
    REMSET_CONSUME = 3,
    REMSET_REGION_CLEAR = 4,
    REMSET_TRANSFER_OUT = 5,
    REMSET_STORE = 6,
};

namespace RootLedger {
constexpr size_t kSlots = 1u << 18;
constexpr size_t kMask = kSlots - 1;
struct Record {
    std::atomic<MAddress> object{ 0 };
    std::atomic<uint64_t> hits{ 0 };
    std::atomic<uint32_t> gc{ 0 };
    std::atomic<MAddress> slot{ 0 };
    std::atomic<uint8_t> kind{ 0 };
};
inline Record* Records()
{
    static Record records[kSlots];
    return records;
}
inline size_t Hash(MAddress address)
{
    return static_cast<size_t>((address * 11400714819323198485ull) >> (64 - 18));
}
} // namespace RootLedger

namespace EdgeStoreLedger {
constexpr size_t kSlots = 1u << 18;
constexpr size_t kMask = kSlots - 1;
struct Record {
    std::atomic<MAddress> slot{ 0 };
    std::atomic<uint64_t> writes{ 0 };
    std::atomic<MAddress> target{ 0 };
    std::atomic<MAddress> previous{ 0 };
    std::atomic<uint8_t> barrierPhase{ 0 };
    std::atomic<uint8_t> slowPath{ 0 };
    std::atomic<uint64_t> remsetEpoch{ 0 };
    std::atomic<uint64_t> remsetRecords{ 0 };
    std::atomic<uint64_t> remsetConsumes{ 0 };
    std::atomic<uint64_t> remsetClears{ 0 };
    std::atomic<MAddress> remsetTarget{ 0 };
    std::atomic<uint8_t> remsetEvent{ 0 };
    std::atomic<uint8_t> remsetFace{ 0 };
};
inline Record* Records()
{
    static Record records[kSlots];
    return records;
}
inline size_t Hash(MAddress address)
{
    return static_cast<size_t>((address * 11400714819323198485ull) >> (64 - 18));
}
} // namespace EdgeStoreLedger

inline bool RootTrackingEnabled();

inline std::atomic<uint64_t>& RemsetEpoch()
{
    static std::atomic<uint64_t> epoch { 0 };
    return epoch;
}

inline void NoteRemsetFlip()
{
    if (RootTrackingEnabled()) {
        RemsetEpoch().fetch_add(1, std::memory_order_relaxed);
    }
}

inline bool RootTrackingEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("MRT_GCV2_NWREAD");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline void NoteRoot(MAddress object, MAddress slot, uint32_t gc, RootKind kind)
{
    if (!RootTrackingEnabled() || object == 0) {
        return;
    }
    const size_t start = RootLedger::Hash(object);
    for (size_t n = 0; n < 8; ++n) {
        RootLedger::Record& record = RootLedger::Records()[(start + n) & RootLedger::kMask];
        MAddress current = record.object.load(std::memory_order_relaxed);
        if (current == 0) {
            MAddress expected = 0;
            if (record.object.compare_exchange_strong(expected, object, std::memory_order_relaxed)) {
                current = object;
            } else {
                current = expected;
            }
        }
        if (current == object) {
            record.slot.store(slot, std::memory_order_relaxed);
            record.kind.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
            record.gc.store(gc, std::memory_order_release);
            record.hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

inline RootEvidence LookupRoot(MAddress object)
{
    RootEvidence evidence;
    if (!RootTrackingEnabled() || object == 0) {
        return evidence;
    }
    const size_t start = RootLedger::Hash(object);
    for (size_t n = 0; n < 8; ++n) {
        RootLedger::Record& record = RootLedger::Records()[(start + n) & RootLedger::kMask];
        if (record.object.load(std::memory_order_acquire) == object) {
            evidence.hits = record.hits.load(std::memory_order_relaxed);
            evidence.gc = record.gc.load(std::memory_order_acquire);
            evidence.slot = record.slot.load(std::memory_order_relaxed);
            evidence.kind = static_cast<RootKind>(record.kind.load(std::memory_order_relaxed));
            return evidence;
        }
    }
    return evidence;
}

inline void NoteEdgeStore(MAddress slot, MAddress previous, MAddress target, uint8_t barrierPhase, bool slowPath)
{
    if (!RootTrackingEnabled() || slot == 0) {
        return;
    }
    const size_t start = EdgeStoreLedger::Hash(slot);
    for (size_t n = 0; n < 8; ++n) {
        EdgeStoreLedger::Record& record = EdgeStoreLedger::Records()[(start + n) & EdgeStoreLedger::kMask];
        MAddress current = record.slot.load(std::memory_order_relaxed);
        if (current == 0) {
            MAddress expected = 0;
            if (record.slot.compare_exchange_strong(expected, slot, std::memory_order_relaxed)) {
                current = slot;
            } else {
                current = expected;
            }
        }
        if (current == slot) {
            record.previous.store(previous, std::memory_order_relaxed);
            record.target.store(target, std::memory_order_relaxed);
            record.barrierPhase.store(barrierPhase, std::memory_order_relaxed);
            record.slowPath.store(slowPath ? 1u : 0u, std::memory_order_relaxed);
            record.writes.fetch_add(1, std::memory_order_release);
            return;
        }
    }
}

inline EdgeStoreEvidence LookupEdgeStore(MAddress slot)
{
    EdgeStoreEvidence evidence;
    if (!RootTrackingEnabled() || slot == 0) {
        return evidence;
    }
    const size_t start = EdgeStoreLedger::Hash(slot);
    for (size_t n = 0; n < 8; ++n) {
        EdgeStoreLedger::Record& record = EdgeStoreLedger::Records()[(start + n) & EdgeStoreLedger::kMask];
        if (record.slot.load(std::memory_order_acquire) == slot) {
            evidence.writes = record.writes.load(std::memory_order_acquire);
            evidence.target = record.target.load(std::memory_order_relaxed);
            evidence.previous = record.previous.load(std::memory_order_relaxed);
            evidence.barrierPhase = record.barrierPhase.load(std::memory_order_relaxed);
            evidence.slowPath = record.slowPath.load(std::memory_order_relaxed);
            evidence.remsetEpoch = record.remsetEpoch.load(std::memory_order_relaxed);
            evidence.remsetRecords = record.remsetRecords.load(std::memory_order_relaxed);
            evidence.remsetConsumes = record.remsetConsumes.load(std::memory_order_relaxed);
            evidence.remsetClears = record.remsetClears.load(std::memory_order_relaxed);
            evidence.remsetTarget = record.remsetTarget.load(std::memory_order_relaxed);
            evidence.remsetEvent = record.remsetEvent.load(std::memory_order_relaxed);
            evidence.remsetFace = record.remsetFace.load(std::memory_order_relaxed);
            return evidence;
        }
    }
    return evidence;
}

inline void NoteRemsetEvent(MAddress slot, RemsetEvent event, uint8_t face, MAddress target = 0)
{
    if (!RootTrackingEnabled() || slot == 0) {
        return;
    }
    const size_t start = EdgeStoreLedger::Hash(slot);
    for (size_t n = 0; n < 8; ++n) {
        EdgeStoreLedger::Record& record = EdgeStoreLedger::Records()[(start + n) & EdgeStoreLedger::kMask];
        if (record.slot.load(std::memory_order_acquire) != slot) {
            continue;
        }
        record.remsetEpoch.store(RemsetEpoch().load(std::memory_order_relaxed), std::memory_order_relaxed);
        record.remsetTarget.store(target, std::memory_order_relaxed);
        record.remsetFace.store(face, std::memory_order_relaxed);
        record.remsetEvent.store(static_cast<uint8_t>(event), std::memory_order_release);
        if (event == REMSET_MUTATOR_RECORD || event == REMSET_GC_RECORD) {
            record.remsetRecords.fetch_add(1, std::memory_order_relaxed);
        } else if (event == REMSET_CONSUME) {
            record.remsetConsumes.fetch_add(1, std::memory_order_relaxed);
        } else if (event == REMSET_REGION_CLEAR) {
            record.remsetClears.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}
} // namespace ProbeReadRouteDiag
} // namespace MapleRuntime

#endif // MRT_PROBE_READ_ROUTE_DIAG_H
