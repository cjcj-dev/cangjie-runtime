// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstdint>
#include <cstdio>
#include <limits>

#include "Common/ColourEncoding.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* name)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}
} // namespace

int main()
{
    using namespace MapleRuntime;
    constexpr uintptr_t lowStart = uintptr_t(1) << 40;
    Expect(IsRepresentableLow48Range(lowStart, 4096), "low48-normal-range");
    Expect(IsRepresentableLow48Range(kPointerAddressLimit - 4096, 4096), "low48-exclusive-limit");
    Expect(!IsRepresentableLow48Range(kPointerAddressLimit, 1), "low48-high-start-rejected");
    Expect(!IsRepresentableLow48Range(kPointerAddressLimit - 4096, 4097), "low48-high-end-rejected");
    Expect(!IsRepresentableLow48Range(std::numeric_limits<uintptr_t>::max() - 1, 8),
           "low48-overflowing-end-rejected");
    size_t result = 0;
    Expect(CheckedMulSize(4096, 1024, result) && result == 4 * 1024 * 1024,
           "checked-mul-normal");
    Expect(!CheckedMulSize(std::numeric_limits<size_t>::max(), 2, result),
           "checked-mul-overflow");
    Expect(CheckedAddSize(4096, 8192, result) && result == 12288,
           "checked-add-normal");
    Expect(!CheckedAddSize(std::numeric_limits<size_t>::max(), 1, result),
           "checked-add-overflow");
    Expect(CheckedRoundUpSize(4097, 4096, result) && result == 8192,
           "checked-roundup-normal");
    Expect(!CheckedRoundUpSize(std::numeric_limits<size_t>::max(), 4096, result),
           "checked-roundup-overflow");
    Expect(IsAddressLayoutSealValid(HeapSlotAddressRange{ lowStart, lowStart + 4096 },
                                           StateWordTypeInfoRange{ lowStart + 8192, lowStart + 12288 }),
           "address-layout-seal-valid");
    Expect(!IsAddressLayoutSealValid(HeapSlotAddressRange{ lowStart, kPointerAddressLimit + 1 },
                                            StateWordTypeInfoRange{ lowStart + 8192, lowStart + 12288 }),
           "address-layout-seal-high-heap-rejected");
    Expect(!IsAddressLayoutSealValid(HeapSlotAddressRange{ lowStart, lowStart + 4096 },
                                            StateWordTypeInfoRange{ lowStart + 8192,
                                                                    kPointerAddressLimit + 1 }),
           "address-layout-seal-high-typeinfo-rejected");
    const uintptr_t address = 0x12345000;
    const uintptr_t storeGood = address | ZPointerRemapped00 | MARKED_YOUNG_0 |
        MARKED_OLD_0 | REMEMBERED_0;
    const uintptr_t staleLoadBad = address | ZPointerRemapped01 | MARKED_YOUNG_0 |
        MARKED_OLD_0 | REMEMBERED_0;
    Expect(ClassifySlotWord(0) == SlotWordVerdict::kNull, "slot-null");
    Expect(ClassifySlotWord(address) == SlotWordVerdict::kLegacyPlain, "slot-legacy-plain");
    Expect(ClassifySlotWord(storeGood) == SlotWordVerdict::kColoured, "slot-store-good");
    Expect(ClassifySlotWord(staleLoadBad) == SlotWordVerdict::kColoured, "slot-stale-load-bad");
    Expect(ClassifySlotWord(address | ZPointerRemapped00 | ZPointerRemapped01) == SlotWordVerdict::kIllegal,
           "slot-illegal-remap-popcount");
    Expect(ClassifySlotWord(address | MARKED_YOUNG_0 | MARKED_YOUNG_1) == SlotWordVerdict::kIllegal,
           "slot-illegal-marked-young-popcount");
    Expect(ClassifySlotWord(address | MARKED_OLD_0 | MARKED_OLD_1) == SlotWordVerdict::kIllegal,
           "slot-illegal-marked-old-popcount");
    Expect(ClassifySlotWord(address | REMEMBERED_0 | REMEMBERED_1) == SlotWordVerdict::kIllegal,
           "slot-illegal-remembered-popcount");
    Expect(ClassifySlotWord(address | FINALIZABLE_0) == SlotWordVerdict::kIllegal,
           "slot-illegal-unwired-finalizable");
    Expect(ClassifySlotWord(uintptr_t(1) << 60) == SlotWordVerdict::kIllegal,
           "slot-illegal-unused-high-bit");
    Expect(ClassifySlotWord(ZPointerRemapped00) == SlotWordVerdict::kIllegal,
           "slot-illegal-colour-without-address");
    if (failures == 0) {
        std::fprintf(stderr, "COLOUR_ENCODING_UNIT_OK tests=25\n");
    }
    return failures == 0 ? 0 : 1;
}
