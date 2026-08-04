// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdlib>
#include <iostream>
#include <unordered_set>

#include "Heap/Barrier/RememberedSet.h"

namespace MapleRuntime {
class RememberedSetTest {
public:
    static void Record(RememberedSet& rememberedSet, MAddress address) { rememberedSet.Record(address); }

    static size_t ClearRegion(RememberedSet& rememberedSet, MAddress start, MAddress end, size_t* words)
    {
        return rememberedSet.ClearRegion(start, end, words);
    }

    static uint8_t BeginFullClear(RememberedSet& rememberedSet) { return rememberedSet.BeginFullClear(); }

    static size_t FinishFullClear(RememberedSet& rememberedSet, uint8_t buffer)
    {
        return rememberedSet.FinishFullClear(buffer);
    }

    static size_t CrossCheckCount(const RememberedSet& rememberedSet)
    {
        std::lock_guard<std::mutex> guard(rememberedSet.oracleLock);
        return rememberedSet.bitmapCrossCheckCount;
    }
};
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    constexpr MAddress start = 0x100000;
    constexpr size_t heapSize = 0x4000;
    constexpr size_t fieldSize = sizeof(RefField<>);

    RememberedSet rememberedSet;
    rememberedSet.Initialize(start, heapSize);
    bool passed = rememberedSet.MemoryOverhead() > 0;

    // Exact boundaries: bit 0, bit 63, and bit 0 of the next bitmap word.
    RememberedSetTest::Record(rememberedSet, start);
    RememberedSetTest::Record(rememberedSet, start + 63 * fieldSize);
    RememberedSetTest::Record(rememberedSet, start + 64 * fieldSize);
    RememberedSetTest::Record(rememberedSet, start + 64 * fieldSize);
    passed = rememberedSet.Size() == 3 && rememberedSet.Contains(start) &&
             rememberedSet.Contains(start + 63 * fieldSize) &&
             rememberedSet.Contains(start + 64 * fieldSize) && passed;

    std::unordered_set<MAddress> drained;
    passed = rememberedSet.DrainForMinor(drained) == 3 && drained.size() == 3 && rememberedSet.Size() == 0 && passed;

    // Region cleanup is half-open and touches both owned bitmap slices.
    RememberedSetTest::Record(rememberedSet, start + 127 * fieldSize);
    RememberedSetTest::Record(rememberedSet, start + 128 * fieldSize);
    size_t words = 0;
    passed = RememberedSetTest::ClearRegion(
                 rememberedSet, start, start + 128 * fieldSize, &words) == 1 &&
             words == 4 && !rememberedSet.Contains(start + 127 * fieldSize) &&
             rememberedSet.Contains(start + 128 * fieldSize) && passed;

    // A full-GC rotation drops the captured old buffer and preserves writes made
    // after the clean buffer has been published.
    RememberedSetTest::Record(rememberedSet, start + 192 * fieldSize);
    uint8_t oldBuffer = RememberedSetTest::BeginFullClear(rememberedSet);
    RememberedSetTest::Record(rememberedSet, start + 256 * fieldSize);
    passed = RememberedSetTest::FinishFullClear(rememberedSet, oldBuffer) == 2 &&
             rememberedSet.Size() == 1 && rememberedSet.Contains(start + 256 * fieldSize) && passed;

    // Product removes static slots from the heap bitmap; validation proves that the
    // independently scanned static-root channel visits every former set element.
    constexpr MAddress staticSlot = 0x80000;
    rememberedSet.RecordStaticForCrossCheck(staticSlot);
    rememberedSet.VisitStaticForCrossCheck(staticSlot);
    rememberedSet.CheckStaticCoverageForMinor();

    size_t checks = RememberedSetTest::CrossCheckCount(rememberedSet);
    passed = checks == 3 && passed;
    std::cout << "EQUIVALENCE_BITMAP checks=" << checks << "/3 precision=1field/bit result="
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
