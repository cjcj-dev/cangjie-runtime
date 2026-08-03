// Observation-only probe for 1-bit tagID epoch ABA hypothesis (lane gctagid).
// Does not change tag/untag/flip semantics.
#ifndef MRT_TAG_EPOCH_PROBE_H
#define MRT_TAG_EPOCH_PROBE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;

// Bounded side-table: field address -> last tag-write epoch snapshot.
// Strategy: open-addressed hash, fixed capacity, overwrite on collision.
struct TagEpochProbe {
    static constexpr size_t kCapacity = 1u << 16; // 65536 slots
    static constexpr size_t kDeltaBuckets = 8;    // delta 0..6, 7+ / MISS

    struct Entry {
        std::atomic<uintptr_t> fieldAddr{0};
        std::atomic<uint64_t> majorEpoch{0};
        std::atomic<uint64_t> minorEpoch{0};
        std::atomic<uint32_t> tagID{0};
        std::atomic<uint32_t> phase{0};
        std::atomic<uint32_t> writerKind{0};
        std::atomic<uint64_t> seq{0};
    };

    enum WriterKind : uint32_t {
        WK_UNKNOWN = 0,
        WK_SET_FIELD = 1,
        WK_COMPARE_EXCHANGE = 2,
        WK_EXCHANGE = 3,
    };

    static std::atomic<uint64_t> majorEpoch;
    static std::atomic<uint64_t> minorEpoch;
    static std::atomic<uint64_t> minorSinceLastMajor;
    static std::atomic<uint64_t> majorTotal;
    static std::atomic<uint64_t> minorTotal;
    static std::atomic<uint64_t> tagWriteCount;
    static std::atomic<uint64_t> probeFireCount;
    static std::atomic<uint64_t> controlFireCount;
    static std::atomic<uint64_t> staleCrashCount;
    static std::atomic<uint64_t> deltaHist[kDeltaBuckets];
    static std::atomic<uint64_t> writerPhaseHist[16];
    static std::atomic<uint64_t> minorPerMajorHist[16];
    static std::atomic<uint64_t> lastCrashMinorSinceMajor;
    static std::atomic<uint64_t> lastCrashDelta;
    static std::atomic<uint32_t> lastCrashPhase;
    static std::atomic<uint32_t> lastCrashTagID;
    static std::atomic<uint64_t> lastCrashTagMajor;
    static std::atomic<uintptr_t> lastCrashField;
    static std::atomic<uintptr_t> lastCrashTarget;
    static Entry table[kCapacity];
    static std::atomic<bool> dumpedOnce;
    static std::atomic<bool> enabled;

    static void InitOnce();
    static void OnMajorFlip();
    static void OnMinorEnd();
    // Called on every ref-field store that may carry a tag bit.
    static void OnFieldWrite(const void* fieldAddr, MAddress newVal, WriterKind kind);
    static void OnTaggedSeen(const void* fieldAddr, MAddress fieldVal, const char* site);
    // Call before FindToVersion when tagged target may leave heap range.
    static void OnPreFindToVersion(const void* fieldAddr, BaseObject* target, MAddress fieldVal, const char* site);
    static void DumpSummary(const char* reason);
    static bool Lookup(uintptr_t fieldAddr, uint64_t& maj, uint64_t& min, uint32_t& tag, uint32_t& phase,
                       uint32_t& kind, uint64_t& seq);

    // x86_64 / aarch64 RefField bit layout (address:48, isTagged:1, tagID:1, padding:14)
    static inline bool ValIsTagged(MAddress v) { return ((v >> 48) & 1u) != 0; }
    static inline uint32_t ValTagID(MAddress v) { return static_cast<uint32_t>((v >> 49) & 1u); }
    static inline MAddress ValAddress(MAddress v) { return v & ((1ULL << 48) - 1ULL); }
};

} // namespace MapleRuntime

#endif
