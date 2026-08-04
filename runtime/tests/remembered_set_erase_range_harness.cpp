// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdlib>
#include <iostream>

#include "Heap/Barrier/RememberedSet.h"

namespace MapleRuntime {
class RememberedSetTest {
public:
    static void Record(RememberedSet& rememberedSet, MAddress address) { rememberedSet.Record(address); }

    static size_t EraseRange(RememberedSet& rememberedSet, MAddress start, MAddress end, size_t* scanned)
    {
        return rememberedSet.EraseRange(start, end, scanned);
    }

    static size_t CrossCheckCount(const RememberedSet& rememberedSet)
    {
        std::lock_guard<std::mutex> guard(rememberedSet.lock);
        return rememberedSet.crossCheckCount;
    }
};
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    if (setenv("MRT_GCV2_VERIFY_REMSET_ERASE_RANGE", "1", 1) != 0) {
        return EXIT_FAILURE;
    }

    RememberedSet rememberedSet;
    RememberedSetTest::Record(rememberedSet, 0x100);
    RememberedSetTest::Record(rememberedSet, 0x100);
    RememberedSetTest::Record(rememberedSet, 0x180);
    RememberedSetTest::Record(rememberedSet, 0x1ff);
    RememberedSetTest::Record(rememberedSet, 0x200);
    RememberedSetTest::Record(rememberedSet, 0x500);

    size_t scanned = 1;
    bool passed = RememberedSetTest::EraseRange(rememberedSet, 0x300, 0x300, &scanned) == 0 && scanned == 0;
    scanned = 1;
    passed = RememberedSetTest::EraseRange(rememberedSet, 0x400, 0x300, &scanned) == 0 && scanned == 0 && passed;

    scanned = 0;
    passed = RememberedSetTest::EraseRange(rememberedSet, 0x100, 0x200, &scanned) == 3 && scanned == 5 && passed;
    passed = rememberedSet.Size() == 2 && rememberedSet.Contains(0x200) && rememberedSet.Contains(0x500) && passed;

    scanned = 0;
    passed = RememberedSetTest::EraseRange(rememberedSet, 0x300, 0x400, &scanned) == 0 && scanned == 2 && passed;
    scanned = 0;
    passed = RememberedSetTest::EraseRange(rememberedSet, 0x200, 0x201, &scanned) == 1 && scanned == 2 && passed;
    scanned = 0;
    passed = RememberedSetTest::EraseRange(rememberedSet, 0x0, 0x600, &scanned) == 1 && scanned == 1 && passed;
    passed = rememberedSet.Size() == 0 && passed;

    RememberedSetTest::Record(rememberedSet, 0x700);
    {
        auto records = rememberedSet.AcquireRecordsForMinor();
        passed = records.size() == 1 && passed;
    }
    passed = rememberedSet.Size() == 0 && passed;

    size_t crossChecks = RememberedSetTest::CrossCheckCount(rememberedSet);
    passed = crossChecks == 4 && passed;
    std::cout << "EQUIVALENCE_ERASE_RANGE checks=" << crossChecks << "/4 result="
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
