// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_COLOUR_ENCODING_H
#define MRT_COLOUR_ENCODING_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "Common/ColourMask.h"

namespace MapleRuntime {

// HeapSlot and StateWord both preserve 48 address bits, but they are different
// carriers: HeapSlot high bits are pointer colours while StateWord high bits are
// object state.  Keep range admission below carrier-neutral; typed seal inputs
// prevent callers from feeding a StateWord to the slot-word classifier added by
// the pointer-colour verifier (POINTER_COLOUR_CAMPAIGN R7).
constexpr unsigned kPointerAddressBits = 48u;
constexpr uintptr_t kPointerAddressLimit = uintptr_t(1) << kPointerAddressBits;
constexpr uintptr_t kPointerAddressMask = kPointerAddressLimit - 1u;

struct HeapSlotAddressRange {
    uintptr_t start;
    uintptr_t end;
};

struct StateWordTypeInfoRange {
    uintptr_t start;
    uintptr_t end;
};

// A half-open range [start, start + size) is representable without truncation
// by a 48-bit carrier.  An exclusive end exactly at 2^48 is valid: its final
// represented byte is 2^48-1.  The subtraction form also rejects addition
// overflow without first evaluating the overflowing sum.
__attribute__((visibility("hidden"))) constexpr bool IsRepresentableLow48Range(uintptr_t start, size_t size)
{
    return start < kPointerAddressLimit && size <= kPointerAddressLimit - start;
}

__attribute__((visibility("hidden"))) constexpr bool IsAddressLayoutSealValid(
    HeapSlotAddressRange heap, StateWordTypeInfoRange typeInfo)
{
    return heap.start < heap.end && typeInfo.start < typeInfo.end &&
        heap.end <= kPointerAddressLimit && typeInfo.end <= kPointerAddressLimit;
}

__attribute__((visibility("hidden"))) inline bool CheckedMulSize(size_t left, size_t right, size_t& result)
{
    return !__builtin_mul_overflow(left, right, &result);
}

__attribute__((visibility("hidden"))) inline bool CheckedAddSize(size_t left, size_t right, size_t& result)
{
    return !__builtin_add_overflow(left, right, &result);
}

__attribute__((visibility("hidden"))) inline bool CheckedRoundUpSize(size_t value, size_t alignment, size_t& result)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    size_t biased = 0;
    if (!CheckedAddSize(value, alignment - 1, biased)) {
        return false;
    }
    result = biased & ~(alignment - 1);
    return true;
}

enum class SlotWordVerdict : uint8_t {
    kNull,
    kColoured,
    kIllegal,
};

__attribute__((visibility("hidden"))) constexpr bool ColourFamilyHasExactlyOneBit(
    uintptr_t value, uintptr_t familyMask)
{
    const uintptr_t family = value & familyMask;
    return family != 0 && (family & (family - 1)) == 0;
}

// Single source of truth for the full-colour producer/consumer contract.  A
// non-null HeapSlot word contains exactly one bit from every row.  Tests derive
// both the accepted cardinality and the missing-family negatives from this
// table, so removing a wired family cannot silently weaken the oracle.
constexpr uintptr_t kHeapSlotRequiredColourFamilies[] = {
    REMAP_COLOUR_MASK,
    MARKED_YOUNG_MASK,
    MARKED_OLD_MASK,
    REMEMBERED_MASK,
};
constexpr size_t kHeapSlotRequiredColourFamilyCount =
    sizeof(kHeapSlotRequiredColourFamilies) / sizeof(kHeapSlotRequiredColourFamilies[0]);

__attribute__((visibility("hidden"))) constexpr bool IsPlainNonNullSlotWord(uintptr_t value)
{
    constexpr uintptr_t allMetadata =
        REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK |
        REMEMBERED_MASK | FINALIZABLE_MASK | (uintptr_t(0xf) << 60u);
    return (value & kPointerAddressMask) != 0 && (value & allMetadata) == 0;
}

// Fail-closed admission for the full-colour HeapSlot carrier.  The producer
// matrix has two non-null rows (current store-good and stale-load-bad); both
// write exactly one bit from every wired family.  Keeping the accepted set as
// the conjunction of those family rows makes a removed/partial producer shrink
// the test oracle instead of silently widening this classifier.
__attribute__((visibility("hidden"))) constexpr SlotWordVerdict ClassifySlotWord(uintptr_t value)
{
    if (value == 0) {
        return SlotWordVerdict::kNull;
    }
    constexpr uintptr_t unusedHighMask = uintptr_t(0xf) << 60u;
    if ((value & unusedHighMask) != 0 ||
        (!kFinalizableWired && (value & FINALIZABLE_MASK) != 0) ||
        (value & kPointerAddressMask) == 0) {
        return SlotWordVerdict::kIllegal;
    }
    for (size_t i = 0; i < kHeapSlotRequiredColourFamilyCount; ++i) {
        if (!ColourFamilyHasExactlyOneBit(value, kHeapSlotRequiredColourFamilies[i])) {
            return SlotWordVerdict::kIllegal;
        }
    }
    return SlotWordVerdict::kColoured;
}

// ZAddress::store_good (zAddress.inline.hpp:806-808) for the frozen low-48
// layout.  Used by producers whose payload is a derived/interior address and
// therefore cannot be routed through a BaseObject classifier.
__attribute__((visibility("hidden"))) constexpr uintptr_t MakeStoreGoodSlotWord(
    uintptr_t address, uintptr_t storeGoodMask)
{
    const uintptr_t payload = address & kPointerAddressMask;
    return payload == 0 ? 0 : payload | storeGoodMask;
}

} // namespace MapleRuntime

#endif // MRT_COLOUR_ENCODING_H
