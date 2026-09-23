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

#include "Heap/z/zAddress.hpp"

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

__attribute__((visibility("hidden"))) inline bool IsAddressLayoutSealValid(
    HeapSlotAddressRange heap, StateWordTypeInfoRange typeInfo)
{
    return heap.start < heap.end && typeInfo.start < typeInfo.end &&
        heap.start >= ZAddressHeapBase && heap.end <= ZAddressHeapBase + ZAddressOffsetMax &&
        typeInfo.end <= kPointerAddressLimit;
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


inline bool IsPlainNonNullSlotWord(uintptr_t value)
{
    return value != 0 && (value & ZPointerAllMetadataMask) == 0;
}
inline SlotWordVerdict ClassifySlotWord(uintptr_t value)
{
    if (value == 0) { return SlotWordVerdict::kNull; }
    return is_valid(static_cast<zpointer>(value)) ? SlotWordVerdict::kColoured : SlotWordVerdict::kIllegal;
}
} // namespace MapleRuntime
#endif // MRT_COLOUR_ENCODING_H
