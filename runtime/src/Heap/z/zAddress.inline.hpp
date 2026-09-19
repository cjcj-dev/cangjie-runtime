/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#pragma once
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zGlobals.hpp"
namespace MapleRuntime {
constexpr Uptr raw(zpointer p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress_unsafe p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zoffset p) { return static_cast<Uptr>(p); }
inline uintptr_t untype(zaddress_unsafe p) { return raw(p); }
inline bool is_power_of_2(uintptr_t value) { return value != 0 && (value & (value - 1)) == 0; }
inline uintptr_t ZPointer::remap_bits(uintptr_t value) {
#ifdef __aarch64__
  return (value ^ ZPointerRemappedMask) & ZPointerRemappedMask;
#else
  return value & ZPointerRemappedMask;
#endif
}
inline constexpr int ZPointer::load_shift_lookup(uintptr_t value) {
#ifdef __aarch64__
  return 16;
#else
  const size_t index = (value >> ZPointerRemappedShift) & 0xf;
  assert(index == 0 || index == 1 || index == 2 || index == 4 || index == 8);
  return ZPointerLoadShiftTable[index];
#endif
}
// Offset Operator Macro
// Creates operators for the offset, offset_end style types

#define CREATE_ZOFFSET_OPERATORS(offset_type)                                             \
                                                                                          \
  /* Arithmetic operators for offset_type */                                              \
                                                                                          \
inline offset_type operator+(offset_type offset, size_t size) {                           \
  const auto size_value = static_cast<std::underlying_type_t<offset_type>>(size);        \
  return to_##offset_type(untype(offset) + size_value);                                   \
}                                                                                         \
                                                                                          \
inline offset_type& operator+=(offset_type& offset, size_t size) {                        \
  const auto size_value = static_cast<std::underlying_type_t<offset_type>>(size);        \
  offset = to_##offset_type(untype(offset) + size_value);                                 \
  return offset;                                                                          \
}                                                                                         \
                                                                                          \
inline offset_type operator-(offset_type offset, size_t size) {                           \
  const auto size_value = static_cast<std::underlying_type_t<offset_type>>(size);        \
  return to_##offset_type(untype(offset) - size_value);                                   \
}                                                                                         \
                                                                                          \
inline size_t operator-(offset_type first, offset_type second) {                          \
  return untype(first - untype(second));                                                  \
}                                                                                         \
                                                                                          \
inline offset_type& operator-=(offset_type& offset, size_t size) {                        \
  const auto size_value = static_cast<std::underlying_type_t<offset_type>>(size);        \
  offset = to_##offset_type(untype(offset) - size_value);                                 \
  return offset;                                                                          \
}                                                                                         \
                                                                                          \
  /* Arithmetic operators for offset_type##_end */                                        \
                                                                                          \
inline offset_type##_end operator+(offset_type##_end offset, size_t size) {               \
  const auto size_value = static_cast<std::underlying_type_t<offset_type##_end>>(size);  \
  return to_##offset_type##_end(untype(offset) + size_value);                             \
}                                                                                         \
                                                                                          \
inline offset_type##_end& operator+=(offset_type##_end& offset, size_t size) {            \
  const auto size_value = static_cast<std::underlying_type_t<offset_type##_end>>(size);  \
  offset = to_##offset_type##_end(untype(offset) + size_value);                           \
  return offset;                                                                          \
}                                                                                         \
                                                                                          \
inline offset_type##_end operator-(offset_type##_end first, size_t size) {                \
  const auto size_value = static_cast<std::underlying_type_t<offset_type##_end>>(size);  \
  return to_##offset_type##_end(untype(first) - size_value);                              \
}                                                                                         \
                                                                                          \
inline size_t operator-(offset_type##_end first, offset_type##_end second) {              \
  return untype(first - untype(second));                                                  \
}                                                                                         \
                                                                                          \
inline offset_type##_end& operator-=(offset_type##_end& offset, size_t size) {            \
  const auto size_value = static_cast<std::underlying_type_t<offset_type##_end>>(size);  \
  offset = to_##offset_type##_end(untype(offset) - size_value);                           \
  return offset;                                                                          \
}                                                                                         \
                                                                                          \
  /* Arithmetic operators for offset_type cross offset_type##_end */                      \
                                                                                          \
inline size_t operator-(offset_type##_end first, offset_type second) {                    \
  return untype(first - untype(second));                                                  \
}                                                                                         \
                                                                                          \
  /* Logical operators for offset_type cross offset_type##_end */                         \
                                                                                          \
inline bool operator!=(offset_type first, offset_type##_end second) {                     \
  return untype(first) != untype(second);                                                 \
}                                                                                         \
                                                                                          \
inline bool operator!=(offset_type##_end first, offset_type second) {                     \
  return untype(first) != untype(second);                                                 \
}                                                                                         \
                                                                                          \
inline bool operator==(offset_type first, offset_type##_end second) {                     \
  return untype(first) == untype(second);                                                 \
}                                                                                         \
                                                                                          \
inline bool operator==(offset_type##_end first, offset_type second) {                     \
  return untype(first) == untype(second);                                                 \
}                                                                                         \
                                                                                          \
inline bool operator<(offset_type##_end first, offset_type second) {                      \
  return untype(first) < untype(second);                                                  \
}                                                                                         \
                                                                                          \
inline bool operator<(offset_type first, offset_type##_end second) {                      \
  return untype(first) < untype(second);                                                  \
}                                                                                         \
                                                                                          \
inline bool operator<=(offset_type##_end first, offset_type second) {                     \
  return untype(first) <= untype(second);                                                 \
}                                                                                         \
                                                                                          \
inline bool operator>(offset_type first, offset_type##_end second) {                      \
  return untype(first) > untype(second);                                                  \
}                                                                                         \
                                                                                          \
inline bool operator>=(offset_type first, offset_type##_end second) {                     \
  return untype(first) >= untype(second);                                                 \
}                                                                                         \

// zoffset functions

inline uintptr_t untype(zoffset offset) {
  const uintptr_t value = static_cast<uintptr_t>(offset);
  assert(value < ZAddressOffsetMax);
  return value;
}

inline uintptr_t untype(zoffset_end offset) {
  const uintptr_t value = static_cast<uintptr_t>(offset);
  assert(value <= ZAddressOffsetMax);
  return value;
}

inline zoffset to_zoffset(uintptr_t value) {
  assert(value < ZAddressOffsetMax);
  return zoffset(value);
}

inline zoffset to_zoffset(zoffset_end offset) {
  const uintptr_t value = untype(offset);
  return to_zoffset(value);
}

inline bool to_zoffset_end(zoffset_end* result, zoffset_end start, size_t size) {
  const uintptr_t value = untype(start) + size;
  if (value <= ZAddressOffsetMax) {
    *result = zoffset_end(value);
    return true;
  }
  return false;
}

inline zoffset_end to_zoffset_end(zoffset start, size_t size) {
  const uintptr_t value = untype(start) + size;
  assert(value <= ZAddressOffsetMax);
  return zoffset_end(value);
}

inline zoffset_end to_zoffset_end(uintptr_t value) {
  assert(value <= ZAddressOffsetMax);
  return zoffset_end(value);
}

inline zoffset_end to_zoffset_end(zoffset offset) {
  return zoffset_end(untype(offset));
}

CREATE_ZOFFSET_OPERATORS(zoffset)

// zbacking_offset functions (ZGC zAddress.inline.hpp:198-238)

inline uintptr_t untype(zbacking_offset offset) {
  const uintptr_t value = static_cast<uintptr_t>(offset);
  assert(value < ZBackingOffsetMax);
  return value;
}

inline uintptr_t untype(zbacking_offset_end offset) {
  const uintptr_t value = static_cast<uintptr_t>(offset);
  assert(value <= ZBackingOffsetMax);
  return value;
}

inline zbacking_offset to_zbacking_offset(uintptr_t value) {
  assert(value < ZBackingOffsetMax);
  return zbacking_offset(value);
}

inline zbacking_offset to_zbacking_offset(zbacking_offset_end offset) {
  const uintptr_t value = untype(offset);
  return to_zbacking_offset(value);
}

inline zbacking_offset_end to_zbacking_offset_end(zbacking_offset start, size_t size) {
  const uintptr_t value = untype(start) + size;
  assert(value <= ZBackingOffsetMax);
  return zbacking_offset_end(value);
}

inline zbacking_offset_end to_zbacking_offset_end(uintptr_t value) {
  assert(value <= ZBackingOffsetMax);
  return zbacking_offset_end(value);
}

inline zbacking_offset_end to_zbacking_offset_end(zbacking_offset offset) {
  return zbacking_offset_end(untype(offset));
}

CREATE_ZOFFSET_OPERATORS(zbacking_offset)

// zbacking_index functions (ZGC zAddress.inline.hpp:240-281)

inline uint32_t untype(zbacking_index index) {
  const uint32_t value = static_cast<uint32_t>(index);
  assert(value < ZBackingIndexMax);
  return value;
}

inline uint32_t untype(zbacking_index_end index) {
  const uint32_t value = static_cast<uint32_t>(index);
  assert(value <= ZBackingIndexMax);
  return value;
}

inline zbacking_index to_zbacking_index(uint32_t value) {
  assert(value < ZBackingIndexMax);
  return zbacking_index(value);
}

inline zbacking_index to_zbacking_index(zbacking_index_end index) {
  const uint32_t value = untype(index);
  return to_zbacking_index(value);
}

inline zbacking_index_end to_zbacking_index_end(zbacking_index start, size_t size) {
  const uint32_t start_value = untype(start);
  const uint32_t value = start_value + static_cast<uint32_t>(size);
  assert(value <= ZBackingIndexMax && start_value <= value);
  return zbacking_index_end(value);
}

inline zbacking_index_end to_zbacking_index_end(uint32_t value) {
  assert(value <= ZBackingIndexMax);
  return zbacking_index_end(value);
}

inline zbacking_index_end to_zbacking_index_end(zbacking_index index) {
  return zbacking_index_end(untype(index));
}

CREATE_ZOFFSET_OPERATORS(zbacking_index)

#undef CREATE_ZOFFSET_OPERATORS

// zbacking_offset <-> zbacking_index conversion functions (ZGC zAddress.inline.hpp:285-296).
// Backing storage and virtual addresses share the fixed ZGC granule.

inline zbacking_index to_zbacking_index(zbacking_offset offset) {
  const uintptr_t value = untype(offset);
  assert(value % ZGranuleSize == 0);
  return to_zbacking_index(static_cast<uint32_t>((value >> ZGranuleSizeShift)));
}

inline zbacking_offset to_zbacking_offset(zbacking_index index) {
  const uintptr_t value = untype(index);
  return to_zbacking_offset(value << ZGranuleSizeShift);
}

// ZRange helper functions (ZGC zAddress.inline.hpp:298-314)

inline zoffset to_start_type(zoffset_end offset) {
  return to_zoffset(offset);
}

inline zbacking_index to_start_type(zbacking_index_end offset) {
  return to_zbacking_index(offset);
}

inline zoffset_end to_end_type(zoffset start, size_t size) {
  return to_zoffset_end(start, size);
}

inline zbacking_index_end to_end_type(zbacking_index start, size_t size) {
  return to_zbacking_index_end(start, size);
}
#define report_is_valid_failure(str) assert(!assert_on_failure);

inline bool is_valid(zpointer ptr, bool assert_on_failure = false) {
  if (assert_on_failure && !ZVerifyOops) {
    return true;
  }

  const uintptr_t value = static_cast<uintptr_t>(ptr);

  if (value == 0) {
    // Accept raw null
    return false;
  }

  if ((value & ~ZPointerStoreMetadataMask) != 0) {
#ifndef __aarch64__
    const int index = ZPointer::load_shift_lookup_index(value);
    if (index != 0 && !is_power_of_2(index)) {
      report_is_valid_failure("Invalid remap bits");
      return false;
    }
#endif

    const int shift = ZPointer::load_shift_lookup(value);
    if (!is_power_of_2(value & (ZAddressHeapBase << shift))) {
      report_is_valid_failure("Missing heap base");
      return false;
    }

    if (((value >> shift) & 7) != 0) {
      report_is_valid_failure("Alignment bits should not be set");
      return false;
    }
  }

  const uintptr_t load_metadata = ZPointer::remap_bits(value);
  if (!is_power_of_2(load_metadata)) {
    report_is_valid_failure("Must have exactly one load metadata bit");
    return false;
  }

  const uintptr_t store_metadata = (value & (ZPointerStoreMetadataMask ^ ZPointerLoadMetadataMask));
  const uintptr_t marked_young_metadata = store_metadata & (ZPointerMarkedYoung0 | ZPointerMarkedYoung1);
  const uintptr_t marked_old_metadata = store_metadata & (ZPointerMarkedOld0 | ZPointerMarkedOld1 |
                                                          ZPointerFinalizable0 | ZPointerFinalizable1);
  const uintptr_t remembered_metadata = store_metadata & (ZPointerRemembered0 | ZPointerRemembered1);
  if (!is_power_of_2(marked_young_metadata)) {
    report_is_valid_failure("Must have exactly one marked young metadata bit");
    return false;
  }

  if (!is_power_of_2(marked_old_metadata)) {
    report_is_valid_failure("Must have exactly one marked old metadata bit");
    return false;
  }

  if (remembered_metadata == 0) {
    report_is_valid_failure("Must have at least one remembered metadata bit set");
    return false;
  }

  if ((marked_young_metadata | marked_old_metadata | remembered_metadata) != store_metadata) {
    report_is_valid_failure("Must have exactly three sets of store metadata bits");
    return false;
  }

  if ((value & ZPointerReservedMask) != 0) {
    report_is_valid_failure("Dirty reserved bits");
    return false;
  }

  return true;
}

inline void assert_is_valid(zpointer ptr) {
  #ifndef NDEBUG
  is_valid(ptr, true);
#endif
}

inline uintptr_t untype(zpointer ptr) {
  return static_cast<uintptr_t>(ptr);
}

inline zpointer to_zpointer(uintptr_t value) {
  assert_is_valid(zpointer(value));
  return zpointer(value);
}


inline bool is_null(zpointer ptr) {
  return ptr == zpointer::null;
}

inline bool is_null_any(zpointer ptr) {
  const uintptr_t raw_addr = untype(ptr);
  return (raw_addr & ~ZPointerAllMetadataMask) == 0;
}

// Is it null - colored or not?
inline bool is_null_assert_load_good(zpointer ptr) {
  const bool result = is_null_any(ptr);
  assert(!result || ZPointer::is_load_good(ptr));
  return result;
}

// zaddress functions

inline bool is_null(zaddress addr) {
  return addr == zaddress::null;
}

inline bool is_valid(zaddress addr, bool assert_on_failure = false) {
  if (assert_on_failure && !ZVerifyOops) {
    return true;
  }

  if (is_null(addr)) {
    // Null is valid
    return true;
  }

  const uintptr_t value = static_cast<uintptr_t>(addr);

  if (value & 0x7) {
    // No low order bits
    report_is_valid_failure("Has low-order bits set");
    return false;
  }

  if ((value & ZAddressHeapBase) == 0) {
    // Must have a heap base bit
    report_is_valid_failure("Missing heap base");
    return false;
  }

  if (value >= (ZAddressHeapBase + ZAddressOffsetMax)) {
    // Must not point outside of the heap's virtual address range
    report_is_valid_failure("Address outside of the heap");
    return false;
  }

  return true;
}

inline void assert_is_valid(zaddress addr) {
  #ifndef NDEBUG
  is_valid(addr, true);
#endif
}

inline uintptr_t untype(zaddress addr) {
  return static_cast<uintptr_t>(addr);
}

inline void dereferenceable_test(zaddress addr) {
  if (ZVerifyOops && addr != zaddress::null) { (void)*reinterpret_cast<volatile uintptr_t*>(static_cast<Uptr>(addr)); }
}
inline zaddress to_zaddress(uintptr_t value) {
  const zaddress addr = static_cast<zaddress>(value);
  assert_is_valid(addr);
  dereferenceable_test(addr);
  return addr;
}
inline zaddress_unsafe to_zaddress_unsafe(uintptr_t value) { return static_cast<zaddress_unsafe>(value); }
inline bool is_null(zaddress_unsafe addr) { return addr == zaddress_unsafe::null; }
// ZOffset functions

inline zaddress ZOffset::address(zoffset offset) {
  return to_zaddress(untype(offset) | ZAddressHeapBase);
}

inline zaddress_unsafe ZOffset::address_unsafe(zoffset offset) {
  return to_zaddress_unsafe(untype(offset) | ZAddressHeapBase);
}

// ZPointer functions

inline zaddress ZPointer::uncolor(zpointer ptr) {
  assert(ZPointer::is_load_good(ptr) || is_null_any(ptr));
  const uintptr_t raw_addr = untype(ptr);
  return to_zaddress(raw_addr >> ZPointer::load_shift_lookup(raw_addr));
}

inline zaddress ZPointer::uncolor_store_good(zpointer ptr) {
  assert(ZPointer::is_store_good(ptr));
  return uncolor(ptr);
}

inline zaddress_unsafe ZPointer::uncolor_unsafe(zpointer ptr) {
  assert(ZPointer::is_store_bad(ptr));
  const uintptr_t raw_addr = untype(ptr);
  return to_zaddress_unsafe(raw_addr >> ZPointer::load_shift_lookup(raw_addr));
}

inline bool ZPointer::is_load_bad(zpointer ptr) {
  return untype(ptr) & ZPointerLoadBadMask;
}

inline bool ZPointer::is_load_good(zpointer ptr) {
  return !is_load_bad(ptr) && !is_null(ptr);
}

inline bool ZPointer::is_load_good_or_null(zpointer ptr) {
  // Checking if an address is "not bad" is an optimized version of
  // checking if it's "good or null", which eliminates an explicit
  // null check. However, the implicit null check only checks that
  // the mask bits are zero, not that the entire address is zero.
  // This means that an address without mask bits would pass through
  // the barrier as if it was null. This should be harmless as such
  // addresses should ever be passed through the barrier.
  const bool result = !is_load_bad(ptr);
  assert((is_load_good(ptr) || is_null(ptr)) == result);
  return result;
}

inline bool ZPointer::is_young_load_good(zpointer ptr) {
  assert(!is_null(ptr));
  return (remap_bits(untype(ptr)) & ZPointerRemappedYoungMask) != 0;
}

inline bool ZPointer::is_old_load_good(zpointer ptr) {
  assert(!is_null(ptr));
  return (remap_bits(untype(ptr)) & ZPointerRemappedOldMask) != 0;
}

inline bool ZPointer::is_mark_bad(zpointer ptr) {
  return untype(ptr) & ZPointerMarkBadMask;
}

inline bool ZPointer::is_mark_good(zpointer ptr) {
  return !is_mark_bad(ptr) && !is_null(ptr);
}

inline bool ZPointer::is_mark_good_or_null(zpointer ptr) {
  // Checking if an address is "not bad" is an optimized version of
  // checking if it's "good or null", which eliminates an explicit
  // null check. However, the implicit null check only checks that
  // the mask bits are zero, not that the entire address is zero.
  // This means that an address without mask bits would pass through
  // the barrier as if it was null. This should be harmless as such
  // addresses should ever be passed through the barrier.
  const bool result = !is_mark_bad(ptr);
  assert((is_mark_good(ptr) || is_null(ptr)) == result);
  return result;
}

inline bool ZPointer::is_store_bad(zpointer ptr) {
  return untype(ptr) & ZPointerStoreBadMask;
}

inline bool ZPointer::is_store_good(zpointer ptr) {
  return !is_store_bad(ptr) && !is_null(ptr);
}

inline bool ZPointer::is_store_good_or_null(zpointer ptr) {
  // Checking if an address is "not bad" is an optimized version of
  // checking if it's "good or null", which eliminates an explicit
  // null check. However, the implicit null check only checks that
  // the mask bits are zero, not that the entire address is zero.
  // This means that an address without mask bits would pass through
  // the barrier as if it was null. This should be harmless as such
  // addresses should ever be passed through the barrier.
  const bool result = !is_store_bad(ptr);
  assert((is_store_good(ptr) || is_null(ptr)) == result);
  return result;
}

inline bool ZPointer::is_marked_finalizable(zpointer ptr) {
  assert(!is_null(ptr));
  return untype(ptr) & ZPointerFinalizable;
}

inline bool ZPointer::is_marked_old(zpointer ptr) {
  return untype(ptr) & (ZPointerMarkedOld);
}

inline bool ZPointer::is_marked_young(zpointer ptr) {
  return untype(ptr) & (ZPointerMarkedYoung);
}

inline bool ZPointer::is_marked_any_old(zpointer ptr) {
  return untype(ptr) & (ZPointerMarkedOld |
                        ZPointerFinalizable);
}

inline bool ZPointer::is_remapped(zpointer ptr) {
  assert(!is_null(ptr));
  return remap_bits(untype(ptr)) & ZPointerRemapped;
}

inline bool ZPointer::is_remembered_exact(zpointer ptr) {
  assert(!is_null(ptr));
  return (untype(ptr) & ZPointerRemembered) == ZPointerRemembered;
}

inline constexpr int ZPointer::load_shift_lookup_index(uintptr_t value) {
  return (value >> ZPointerRemappedShift) & ((1 << ZPointerRemappedBits) - 1);
}

// ZAddress functions

inline zpointer ZAddress::color(zaddress addr, uintptr_t color) {
  return to_zpointer((untype(addr) << ZPointer::load_shift_lookup(color)) | color);
}

inline zpointer ZAddress::color(zaddress_unsafe addr, uintptr_t color) {
  return to_zpointer((untype(addr) << ZPointer::load_shift_lookup(color)) | color);
}

inline zoffset ZAddress::offset(zaddress addr) {
  return to_zoffset(untype(addr) & ZAddressOffsetMask);
}

inline zoffset ZAddress::offset(zaddress_unsafe addr) {
  return to_zoffset(untype(addr) & ZAddressOffsetMask);
}

inline zpointer color_null() {
  return ZAddress::color(zaddress::null, ZPointerStoreGoodMask | ZPointerRememberedMask);
}

inline zpointer ZAddress::load_good(zaddress addr, zpointer prev) {
  if (is_null_any(prev)) {
    return color_null();
  }

  const uintptr_t non_load_bits_mask = ZPointerLoadMetadataMask ^ ZPointerAllMetadataMask;
  const uintptr_t non_load_prev_bits = untype(prev) & non_load_bits_mask;
  return color(addr, ZPointerLoadGoodMask | non_load_prev_bits | ZPointerRememberedMask);
}

inline zpointer ZAddress::finalizable_good(zaddress addr, zpointer prev) {
  if (is_null_any(prev)) {
    return color_null();
  }

  return color(addr, ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerFinalizable | ZPointerRememberedMask);
}

inline zpointer ZAddress::mark_good(zaddress addr, zpointer prev) {
  if (is_null_any(prev)) {
    return color_null();
  }

  return color(addr, ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerMarkedOld | ZPointerRememberedMask);
}

inline zpointer ZAddress::mark_old_good(zaddress addr, zpointer prev) {
  if (is_null_any(prev)) {
    return color_null();
  }

  const uintptr_t prev_color = untype(prev);

  const uintptr_t young_marked_mask = ZPointerMarkedYoung0 | ZPointerMarkedYoung1;
  const uintptr_t young_marked = prev_color & young_marked_mask;

  return color(addr, ZPointerLoadGoodMask | ZPointerMarkedOld | young_marked | ZPointerRememberedMask);
}

inline zpointer ZAddress::mark_young_good(zaddress addr, zpointer prev) {
  if (is_null_any(prev)) {
    return color_null();
  }

  const uintptr_t prev_color = untype(prev);

  const uintptr_t old_marked_mask = ZPointerMarkedMask ^ (ZPointerMarkedYoung0 | ZPointerMarkedYoung1);
  const uintptr_t old_marked = prev_color & old_marked_mask;

  return color(addr, ZPointerLoadGoodMask | ZPointerMarkedYoung | old_marked | ZPointerRememberedMask);
}

inline zpointer ZAddress::store_good(zaddress addr) {
  return color(addr, ZPointerStoreGoodMask);
}

inline zpointer ZAddress::store_good_or_null(zaddress addr) {
  return is_null(addr) ? zpointer::null : store_good(addr);
}

inline zaddress safe(zaddress_unsafe value) { return to_zaddress(raw(value)); }
// to_object: 凭什么: sole exit from the colour type system to a C++ object pointer.
// Input must already be zaddress (load-good or proven).
// ⭐ This is the ONLY production site allowed to write reinterpret_cast<BaseObject*>.
// All other paths must go through a named constructor below (or this).
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
extern const bool ZVerifyOops;
void VerifyAccessedOop(zaddress address);
#endif
inline BaseObject* to_object(zaddress a)
{
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    // zAddress.inline.hpp:505-522: verify the actual accessed oop.
    if (ZVerifyOops && a != zaddress::null) { VerifyAccessedOop(a); }
#endif
    return reinterpret_cast<BaseObject*>(raw(a));
}

// from_object: 凭什么: a live BaseObject* in hand is already an uncoloured, safe address.
inline zaddress from_object(const BaseObject* obj)
{
    return to_zaddress(reinterpret_cast<Uptr>(obj));
}

// ── non-RefField origins of BaseObject* (ctyperest exemption set) ─────────
// Bare reinterpret_cast<BaseObject*> outside this header is forbidden
// (tools/check_no_bare_baseobject_cast.sh). Every site picks one of these
// and documents *why* the bits are an uncoloured object base.

// from_region_addr: 凭什么: address computed from region metadata
// (region start / unit / free-slot / route to-addr / walk position).
// Allocator owns the layout; bits are never colour-tagged.
inline BaseObject* from_region_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_alloc_addr: 凭什么: address just returned by the allocator
// (SetClassInfo / NewFinalizer / AllocPinned). Memory is committed and
// about to be / just was initialised as a BaseObject; never coloured.
inline BaseObject* from_alloc_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_native_ref: 凭什么: MCC / Sync / scheduler entry holding a managed
// object as void* / C layout view (CJFuture/CJMutex/…). Protocol: the
// mutator published an uncoloured heap ref; not a RefField load.
// ⚠ Bits are NOT run through a read barrier here — caller must not pass
// a raw field load. Stack-map base pointers (BasePtrType = Uptr) use the
// Uptr overload.
// High 16 bits must be clear (plain address). Coloured bits here mean a
// missing barrier at the call site (traceuncolour: mutexPtr bare load).
inline BaseObject* from_native_ref(Uptr p)
{
    // Address occupies bits 0..47; colour/tag live in 48..63 (RefField.h).
    if (p != 0 && (p >> 48) != 0) {
        // Keep CHECK out of this header (no Log.h); hard fail matches prior OOB.
        __builtin_trap();
    }
    return to_object(to_zaddress(p));
}
inline BaseObject* from_native_ref(const void* p)
{
    return from_native_ref(reinterpret_cast<Uptr>(p));
}

// as_abi_ref_slot: 凭什么: stack buffer used as an ABI sret / arg-register
// placeholder in ArgValue::AddReference. ⛔ NOT a heap object; ⛔ must not
// be treated as a GC root. Exists only so the call stub sees a "ref" slot.
inline BaseObject* as_abi_ref_slot(void* p)
{
    return to_object(to_zaddress(reinterpret_cast<Uptr>(p)));
}


}
