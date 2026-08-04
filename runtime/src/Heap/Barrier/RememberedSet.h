// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMEMBERED_SET_H
#define MRT_REMEMBERED_SET_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
#include <unordered_map>
#endif

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class Barrier;
class WCollector;
class RegionManager;

// Exact old-region field bitmap. The two heap-wide backing arrays are partitioned
// by address: every region owns two disjoint slices, one bit per aligned reference
// field. This keeps region cleanup bounded by the reclaimed region rather than by
// the number of remembered fields in the heap.
class RememberedSet final {
public:
    RememberedSet();
    ~RememberedSet() = default;
    RememberedSet(const RememberedSet&) = delete;
    RememberedSet& operator=(const RememberedSet&) = delete;

    void Initialize(MAddress start, size_t size);

    // Called at the beginning of a minor collection while the mutators are stopped.
    // Atomically makes an empty bitmap active, drains the previous active bitmap into
    // records, and clears it for the next cycle.
    size_t DrainForMinor(std::unordered_set<MAddress>& records);

    // Non-destructive view of the active (next-cycle) records for verification.
    std::unordered_set<MAddress> Snapshot() const;
    bool Contains(MAddress fieldAddress) const;
    size_t Size() const;

    // Bytes reserved by both exact bitmap backings.
    size_t MemoryOverhead() const
    {
        return (wordCount + dirtyWordCount) * sizeof(uint64_t) * kBufferCount;
    }

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    // Validation-only oracle for static/global slots. Product remset storage contains
    // heap slots only; minor GC visits static roots independently every round.
    void RecordStaticForCrossCheck(MAddress fieldAddress, MAddress callsite);
    void VisitStaticForCrossCheck(MAddress fieldAddress);
    void CheckStaticCoverageForMinor();
#endif

private:
    friend class Barrier;
    friend class WCollector;
    friend class RegionManager;
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    friend class RememberedSetTest;
#endif

    static constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8;
    static constexpr size_t kFieldBytes = sizeof(RefField<>);
    static constexpr size_t kBufferCount = 2;

    void Record(MAddress fieldAddress);
    void RecordExternal(MAddress fieldAddress);
    size_t ClearRegion(MAddress start, MAddress end, size_t* outWords = nullptr);
    uint8_t BeginFullClear();
    size_t FinishFullClear(uint8_t scanBuffer);

    size_t AddressToBit(MAddress fieldAddress) const;
    size_t ClearRangeInBuffer(size_t buffer, size_t firstBit, size_t endBit, size_t* outWords);
    size_t ClearBuffer(size_t buffer);
    void MarkWordDirty(size_t buffer, size_t word);
    void ClearWordDirty(size_t buffer, size_t word);
    void CheckInitialized() const;
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    void CheckActiveAgainstOracle(const char* operation) const;
#endif

    MAddress heapStart = 0;
    size_t heapSize = 0;
    size_t bitCount = 0;
    size_t wordCount = 0;
    size_t dirtyWordCount = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> bitmaps[kBufferCount];
    std::unique_ptr<std::atomic<uint64_t>[]> dirtyMaps[kBufferCount];
    std::atomic<size_t> recordCounts[kBufferCount];
    std::atomic<uint8_t> activeBuffer{ 0 };
    bool initialized = false;
    mutable std::mutex externalLock;
    std::unordered_set<MAddress> externalRecords[kBufferCount];

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    mutable std::mutex oracleLock;
    std::unordered_set<MAddress> oracleRecords[kBufferCount];
    std::unordered_set<MAddress> staticRecords;
    std::unordered_map<MAddress, MAddress> staticRecordSites;
    std::unordered_set<MAddress> visitedStaticRoots;
    size_t bitmapCrossCheckCount = 0;
    size_t staticCrossCheckRounds = 0;
    size_t lastDrainedHeapRecords = 0;
#endif
};
} // namespace MapleRuntime
#endif // MRT_REMEMBERED_SET_H
