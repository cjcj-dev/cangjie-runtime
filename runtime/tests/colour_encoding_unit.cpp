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
    Expect(ClassifySlotWord(0) == SlotWordVerdict::kNull, "slot-null");
    Expect(ClassifySlotWord(address) == SlotWordVerdict::kIllegal, "slot-plain-illegal");
    constexpr uintptr_t expectedProducerFamilies[] = {
        REMAP_COLOUR_MASK,
        MARKED_YOUNG_MASK,
        MARKED_OLD_MASK,
        REMEMBERED_MASK,
    };
    Expect(kHeapSlotRequiredColourFamilyCount ==
               sizeof(expectedProducerFamilies) / sizeof(expectedProducerFamilies[0]),
           "slot-producer-manifest-family-count");
    for (size_t i = 0;
         i < kHeapSlotRequiredColourFamilyCount &&
             i < sizeof(expectedProducerFamilies) / sizeof(expectedProducerFamilies[0]);
         ++i) {
        Expect(kHeapSlotRequiredColourFamilies[i] == expectedProducerFamilies[i],
               "slot-producer-manifest-family-row");
    }
    unsigned expectedProducerWords = 1;
    uintptr_t canonicalProducerWord = address;
    for (size_t i = 0; i < kHeapSlotRequiredColourFamilyCount; ++i) {
        const uintptr_t family = kHeapSlotRequiredColourFamilies[i];
        expectedProducerWords *= static_cast<unsigned>(__builtin_popcountll(family));
        canonicalProducerWord |= family & (~family + 1);
    }
    unsigned producerWords = 0;
    for (uintptr_t metadata = 0; metadata < (uintptr_t(1) << 12); ++metadata) {
        if (ClassifySlotWord(address | (metadata << 48u)) == SlotWordVerdict::kColoured) {
            ++producerWords;
        }
    }
    Expect(producerWords == expectedProducerWords, "slot-producer-matrix-cardinality");
    for (size_t i = 0; i < kHeapSlotRequiredColourFamilyCount; ++i) {
        Expect(ClassifySlotWord(canonicalProducerWord & ~kHeapSlotRequiredColourFamilies[i]) ==
                   SlotWordVerdict::kIllegal,
               "slot-producer-matrix-missing-family");
    }
    Expect(ClassifySlotWord(address | ZPointerRemapped00) == SlotWordVerdict::kIllegal,
           "slot-partial-remap-only");
    Expect(ClassifySlotWord(address | ZPointerRemapped00 | MARKED_YOUNG_0 | MARKED_OLD_0) ==
               SlotWordVerdict::kIllegal,
           "slot-partial-missing-remembered");
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
        std::fprintf(stderr, "COLOUR_ENCODING_UNIT_OK producer_words=%u families=%zu\n",
                     producerWords, kHeapSlotRequiredColourFamilyCount);
    }
    return failures == 0 ? 0 : 1;
}
