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
#include <unordered_set>
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
#include <mutex>
#include <unordered_map>
#endif

#include "Common/TypeDef.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class Barrier;
class WCollector;
class RegionManager;
class StoreBarrierBuffer;

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
    // ZGC zRememberedSet.cpp:36 flip(): O(1) current/previous swap. STW1 of
    // YOUNG_CONC_FOLLOW is this plus root handoff (zGeneration.cpp:855-884);
    // the previous face is scanned later by ScanPreviousForMinor.
    void FlipForMinor();
    // zRemembered.cpp:561-576 scan_and_follow: consume the previous face with
    // mutators alive. Callers must FlipForMinor first. DrainForMinor = Flip + Scan.
    size_t ScanPreviousForMinor(std::unordered_set<MAddress>& records);

    // Non-destructive view of the active (next-cycle) records for verification.
    std::unordered_set<MAddress> Snapshot() const;
    bool Contains(MAddress fieldAddress) const;
    size_t Size() const;

    // d1producer: sticky "was this heap field ever handed to Record()" bitmap, never cleared by
    // DrainForMinor. Snapshot() alone cannot separate "the producer never recorded this slot"
    // from "the producer recorded it in an earlier cycle and the destructive drain removed it" --
    // the first is a write-side miss, the second is a retention/consume-side one.
    // Allocated only when MRT_GCV2_REMSET_EVER=1, so the product pays neither memory nor stores.
    // Caveat for readers: slot addresses are reused when a region is recycled, so a true bit may
    // belong to an earlier object at the same address. true is therefore an upper bound on
    // "recorded before" and false is exact: a false bit proves the slot was never recorded.
    bool EverRecordedEnabled() const { return everRecorded != nullptr; }
    bool WasEverRecorded(MAddress fieldAddress) const;

    // Bytes reserved by both exact bitmap backings.
    size_t MemoryOverhead() const
    {
        return (wordCount + dirtyWordCount) * sizeof(uint64_t) * kBufferCount;
    }

#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    // Validation-only correlation between non-heap records and roots visited in the
    // same minor round. Product storage retains these records independently.
    void RecordStaticForCrossCheck(MAddress fieldAddress, MAddress callsite);
    void VisitStaticForCrossCheck(MAddress fieldAddress);
    void CheckStaticCoverageForMinor();
#endif

private:
    friend class Barrier;
    friend class WCollector;
    friend class RegionManager;
    friend class StoreBarrierBuffer;
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    friend class RememberedSetTest;
#endif

    static constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8;
    static constexpr size_t kFieldBytes = sizeof(RefField<>);
    static constexpr size_t kBufferCount = 2;

    // fromMutatorBarrier: true only on the write-barrier path (Barrier::RecordCrossGenEdge).
    // The conservative pinned/old walk and the promotion replay also call Record(), and counting
    // those in the sticky bitmap would answer a different question than "did the producer record it".
    void Record(MAddress fieldAddress, bool fromMutatorBarrier = false);
    // ZGC update_remset_old_to_old (zRelocate.cpp:652-731): move remset bits covering
    // [fromBase, fromBase+size) to the same field offsets under toBase. Does not clear
    // the from range — CollectRegion → ClearRegion scrubs the whole from region.
    // Returns the number of bits recorded at to-addresses (0 if fromBase==toBase).
    size_t TransferObjectSlots(MAddress fromBase, MAddress toBase, size_t size);
    size_t ClearRegion(MAddress start, MAddress end, size_t* outWords = nullptr);
    uint8_t BeginFullClear();
    size_t FinishFullClear(uint8_t scanBuffer);

    size_t AddressToBit(MAddress fieldAddress) const;
    size_t ClearRangeInBuffer(size_t buffer, size_t firstBit, size_t endBit, size_t* outWords);
    size_t ClearBuffer(size_t buffer);
    void MarkWordDirty(size_t buffer, size_t word);
    void ClearWordDirty(size_t buffer, size_t word);
    void CheckInitialized() const;
    MAddress heapStart = 0;
    size_t heapSize = 0;
    size_t bitCount = 0;
    size_t wordCount = 0;
    size_t dirtyWordCount = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> bitmaps[kBufferCount];
    std::unique_ptr<std::atomic<uint64_t>[]> dirtyMaps[kBufferCount];
    // d1producer: single, never-cleared backing for WasEverRecorded. null unless gated on.
    std::unique_ptr<std::atomic<uint64_t>[]> everRecorded;
    std::atomic<size_t> recordCounts[kBufferCount];
    std::atomic<uint8_t> activeBuffer{ 0 };
    bool initialized = false;

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
