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
constexpr bool IsRepresentableLow48Range(uintptr_t start, size_t size)
{
    return start < kPointerAddressLimit && size <= kPointerAddressLimit - start;
}

constexpr bool IsAddressLayoutSealValid(HeapSlotAddressRange heap, StateWordTypeInfoRange typeInfo)
{
    return heap.start < heap.end && typeInfo.start < typeInfo.end &&
        heap.end <= kPointerAddressLimit && typeInfo.end <= kPointerAddressLimit;
}

inline bool CheckedMulSize(size_t left, size_t right, size_t& result)
{
    return !__builtin_mul_overflow(left, right, &result);
}

inline bool CheckedAddSize(size_t left, size_t right, size_t& result)
{
    return !__builtin_add_overflow(left, right, &result);
}

inline bool CheckedRoundUpSize(size_t value, size_t alignment, size_t& result)
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
    kLegacyPlain,
    kColoured,
    kIllegal,
};

constexpr bool ColourFamilyHasMultipleBits(uintptr_t value, uintptr_t familyMask)
{
    const uintptr_t family = value & familyMask;
    return family != 0 && (family & (family - 1)) != 0;
}

// Structural admission for the HeapSlot carrier.  This deliberately does not
// require every metadata family: migration can leave weaker but structurally
// producible colours.  It rejects only words no enumerated producer can make:
// ColourStoreGood and ColourStaleLoadBad choose at most one bit from each live
// family; StoreColoured and self-heal preserve that shape.  Like ZGC's complete
// pointer validation (zAddress.inline.hpp:320-389), a mask hit alone is not an
// encoding proof.
constexpr SlotWordVerdict ClassifySlotWord(uintptr_t value)
{
    if (value == 0) {
        return SlotWordVerdict::kNull;
    }
    constexpr uintptr_t liveColourMask =
        REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK | REMEMBERED_MASK;
    constexpr uintptr_t unusedHighMask = uintptr_t(0xf) << 60u;
    if ((value & unusedHighMask) != 0 ||
        (!kFinalizableWired && (value & FINALIZABLE_MASK) != 0) ||
        ColourFamilyHasMultipleBits(value, REMAP_COLOUR_MASK) ||
        ColourFamilyHasMultipleBits(value, MARKED_YOUNG_MASK) ||
        ColourFamilyHasMultipleBits(value, MARKED_OLD_MASK) ||
        ColourFamilyHasMultipleBits(value, REMEMBERED_MASK)) {
        return SlotWordVerdict::kIllegal;
    }
    const bool hasAddress = (value & kPointerAddressMask) != 0;
    const bool hasColour = (value & liveColourMask) != 0;
    if (!hasAddress) {
        return SlotWordVerdict::kIllegal;
    }
    return hasColour ? SlotWordVerdict::kColoured : SlotWordVerdict::kLegacyPlain;
}

// One runtime gate backs both the heap-write validator and the safe-point
// census.  The definition owns the one-shot ARMED and process-exit summaries.
bool ColouredWritesArmed();

} // namespace MapleRuntime

#endif // MRT_COLOUR_ENCODING_H
