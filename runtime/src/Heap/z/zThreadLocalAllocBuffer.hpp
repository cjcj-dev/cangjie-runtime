// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOC_BUFFER_H
#define MRT_ALLOC_BUFFER_H

#include <algorithm>
#include <functional>

#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zMarkStackEntry.hpp"


#include "Heap/z/zValue.hpp"
#include "Heap/z/zPageFwd.hpp"
#include "Base/Globals.h"
namespace MapleRuntime {
class TLABAllocationAverage {
public:
    void Sample(double value)
    {
        samples = std::min(samples + 1, 100u);
        const unsigned weight = std::max(35u, 100u / samples);
        average = ((100 - weight) * average + weight * value) / 100.0;
    }
    double Average() const { return average; }
private:
    unsigned samples = 0;
    double average = 0;
};

// ThreadLocalAllocStats (threadLocalAllocBuffer.cpp:346-412), in bytes.
struct TLABStatistics {
    size_t allocatedSize = 0;
    size_t refillWaste = 0;
    size_t gcWaste = 0;
    size_t refills = 0;
    size_t slowAllocations = 0;
    size_t allocatingThreads = 0;

    size_t Used() const { return allocatedSize - refillWaste - gcWaste; }
    void Update(const TLABStatistics& other)
    {
        allocatedSize += other.allocatedSize;
        refillWaste += other.refillWaste;
        gcWaste += other.gcWaste;
        refills += other.refills;
        slowAllocations += other.slowAllocations;
        allocatingThreads += other.allocatingThreads;
    }
};


class Mutator;
// ZGC zThreadLocalAllocBuffer.hpp:31-46: worker statistics are distinct
// from the active thread's ThreadLocalAllocBuffer and its watermark snapshot.
class ZThreadLocalAllocBuffer {
public:
    static void initialize();
    static void reset_statistics();
    static void publish_statistics();
    static void retire(Mutator& thread, TLABStatistics& stats);
    static void update_stats(Mutator& thread);
private:
    static ZPerWorker<TLABStatistics>* statistics;
};

// HotSpot gcUtil.cpp:29-56, TLABAllocationWeight=35. Startup samples
// use 1/n until the configured weight dominates.
class AllocBuffer {
public:
    AllocBuffer() = default;
    ~AllocBuffer();
    void Init();
    void Fini();
    static AllocBuffer* GetAllocBuffer();

    MAddress Allocate(size_t size, AllocType allocType);
    MAddress AllocateImpl(size_t totalSize, AllocType allocType);
    ZPage* GetRegion() const;
    size_t TLABSize() const { return tlab.end - tlab.start; }
    void FillTLAB(uintptr_t start, size_t size);
    void ClearRegion();

    size_t RefillWasteLimit() const { return refillWasteLimit; }
    size_t InitialRefillWasteLimit() const;
    static size_t RefillWasteLimitIncrement();
    void RecordSlowAllocation(size_t objectSize);

    size_t ComputeTLABSize(size_t objectSize, size_t maxSize) const;
    void AccumulateTLABStatistics(TLABStatistics& total, size_t used, size_t capacity);
    void ResizeTLAB(size_t capacity, double fallbackFraction, size_t maxSize);



    void FlushRegion();
    void RetireTLAB(bool gcWaste);

private:

    // Inline TLAB bounds, as in HotSpot ThreadLocalAllocBuffer.
    // Compiler offsets are checked by check-cangjie-tlab-layout.py.
    struct TLAB {
        uintptr_t top = 0;
        uintptr_t end = 0;
        uintptr_t start = 0;
    };
    TLAB tlab;
    bool initialized = false;
    static constexpr size_t MinTLABSize = 2 * 1024;
    uintptr_t AllocateInTLAB(size_t size);

    // HotSpot ThreadLocalAllocBuffer: thread-owned statistics survive refills
    // and reset only at a young-cycle boundary.
    size_t refillWasteLimit = 0;
    TLABStatistics tlabStatistics;
    TLABAllocationAverage tlabAllocationFraction;
    std::atomic<size_t> desiredTLABSize{ MinTLABSize };
    std::atomic<size_t> tlabRefills{ 0 };
};
} // namespace MapleRuntime
#endif // MRT_ALLOC_BUFFER_H
