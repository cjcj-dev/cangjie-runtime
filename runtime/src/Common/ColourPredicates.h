// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_COLOUR_PREDICATES_H
#define MRT_COLOUR_PREDICATES_H

#include "Common/ColourMask.h"

// Pure predicates over a coloured heap-slot word.
//
// Shape and names follow OpenJDK ZPointer (zAddress.hpp:267-287 and
// zAddress.inline.hpp:627-725). Unlike ZPointer, these functions take the
// currently published bad mask explicitly. They never read GCPhase or a
// collector object, and they do not cache another copy of the current epoch.
// Product callers must pass g_cjLoadBadMask/g_cjMarkBadMask/
// g_cjStoreBadMask, as appropriate; compile-time callers use the fields of
// ComputeBadMasks(EpochColours). The published masks therefore remain the
// only run-time truth.
//
// Our heap still contains deliberately plain RootSlot/DerivedSlot values and
// is part-way through eliminating plain HeapSlot values. ZGC can implement
// *_good_or_null as merely "not bad" because every non-null oop is coloured.
// Doing that here would newly admit a plain non-null word. The good predicates
// consequently require both a payload address and the current remap bit. This
// preserves Collector::is_load_good's existing fail-closed rule while keeping
// the decision a pure colour-bit test.

namespace MapleRuntime {
namespace ColourPredicates {

constexpr unsigned HEAP_ADDRESS_BITS = 48u;
constexpr uintptr_t HEAP_ADDRESS_MASK = (uintptr_t(1) << HEAP_ADDRESS_BITS) - 1u;

constexpr bool has_address(uintptr_t value)
{
    return (value & HEAP_ADDRESS_MASK) != 0;
}

// The current combined RemappedYoung x RemappedOld bit is the only remap bit
// excluded from load-bad. This derives it from the compiler ABI mask instead
// of publishing a second current-remap global.
constexpr uintptr_t current_remapped(uintptr_t loadBadMask)
{
    return REMAP_COLOUR_MASK & ~loadBadMask;
}

constexpr uintptr_t current_remapped_young_mask(uintptr_t loadBadMask)
{
    const uintptr_t current = current_remapped(loadBadMask);
    return (current == ZPointerRemapped00 || current == ZPointerRemapped10)
        ? (ZPointerRemapped00 | ZPointerRemapped10)
        : (current == ZPointerRemapped01 || current == ZPointerRemapped11)
            ? (ZPointerRemapped01 | ZPointerRemapped11)
            : uintptr_t(0);
}

constexpr uintptr_t current_remapped_old_mask(uintptr_t loadBadMask)
{
    const uintptr_t current = current_remapped(loadBadMask);
    return (current == ZPointerRemapped00 || current == ZPointerRemapped01)
        ? (ZPointerRemapped00 | ZPointerRemapped01)
        : (current == ZPointerRemapped10 || current == ZPointerRemapped11)
            ? (ZPointerRemapped10 | ZPointerRemapped11)
            : uintptr_t(0);
}

constexpr uintptr_t current_marked_young(uintptr_t markBadMask)
{
    return MARKED_YOUNG_MASK & ~markBadMask;
}

constexpr uintptr_t current_marked_old(uintptr_t markBadMask)
{
    return MARKED_OLD_MASK & ~markBadMask;
}

// Finalizable is reserved but not yet published (kFinalizableWired == false).
// ZGC flips it with MarkedOld. Deriving the reserved current bit from the live
// MarkedOld epoch lets the predicate and its tests exist without pretending
// that any product phase currently emits the bit.
constexpr uintptr_t current_finalizable(uintptr_t markBadMask)
{
    const uintptr_t markedOld = current_marked_old(markBadMask);
    return markedOld == MARKED_OLD_0 ? FINALIZABLE_0
        : markedOld == MARKED_OLD_1 ? FINALIZABLE_1 : uintptr_t(0);
}

constexpr uintptr_t current_remembered(uintptr_t storeBadMask)
{
    return REMEMBERED_MASK & ~storeBadMask;
}

// ZPointer::is_load_bad -- true when TAGGED_BITS_MASK or a non-current remap
// bit is present. A plain word is not mask-bad, but is_load_good below still
// rejects it because it has no current remap bit. The answer can change when
// the remap epoch flips in GC_PHASE_PREFORWARD.
constexpr bool is_load_bad(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & loadBadMask) != 0;
}

// ZPointer::is_remapped -- current combined remap epoch. Changes at relocate
// start (our GC_PHASE_PREFORWARD paths).
constexpr bool is_remapped(uintptr_t value, uintptr_t loadBadMask)
{
    const uintptr_t remapped = current_remapped(loadBadMask);
    return remapped != 0 && (value & remapped) != 0;
}

// ZPointer::is_load_good -- a non-null heap payload with the current combined
// remap bit and no tagged/stale-remap bit. It is phase-independent; the answer
// changes only when g_cjLoadBadMask is republished at relocate start.
constexpr bool is_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return has_address(value) && !is_load_bad(value, loadBadMask) && is_remapped(value, loadBadMask);
}

constexpr bool is_load_good_or_null(uintptr_t value, uintptr_t loadBadMask)
{
    return value == 0 || is_load_good(value, loadBadMask);
}

// ZPointer::is_load_good_or_null above is true only for raw null or strict
// load-good; it deliberately rejects a plain non-null word in every phase.
//
// ZPointer::is_young_load_good/is_old_load_good -- true when the word's remap
// bit belongs to the current conceptual young/old half of the four-way colour.
// The halves change at the generation's relocate start in
// GC_PHASE_PREFORWARD. Both masks are derived from g_cjLoadBadMask; no phase
// or duplicate epoch word is consulted.
constexpr bool is_young_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & current_remapped_young_mask(loadBadMask)) != 0;
}

constexpr bool is_old_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & current_remapped_old_mask(loadBadMask)) != 0;
}

// ZPointer::is_mark_bad -- true for any load-bad bit or a stale
// MarkedYoung/MarkedOld bit. Mark epochs change around GC_PHASE_ENUM and remap
// epochs change during GC_PHASE_PREFORWARD.
constexpr bool is_mark_bad(uintptr_t value, uintptr_t markBadMask)
{
    return (value & markBadMask) != 0;
}

// ZPointer::is_mark_good -- load-good plus current MarkedYoung/MarkedOld.
// Those mark epochs flip at GC_PHASE_ENUM; no phase read participates here.
constexpr bool is_mark_good(uintptr_t value, uintptr_t loadBadMask, uintptr_t markBadMask)
{
    return is_load_good(value, loadBadMask) && !is_mark_bad(value, markBadMask) &&
        (value & current_marked_young(markBadMask)) != 0 && (value & current_marked_old(markBadMask)) != 0;
}

constexpr bool is_mark_good_or_null(uintptr_t value, uintptr_t loadBadMask, uintptr_t markBadMask)
{
    return value == 0 || is_mark_good(value, loadBadMask, markBadMask);
}

// ZPointer::is_mark_good_or_null above is true only for raw null or a word
// carrying current remap + MarkedYoung + MarkedOld bits. It follows the ENUM
// mark flips and PREFORWARD remap flips through the explicit masks.
//
// ZPointer::is_store_bad -- true for any mark-bad bit or a stale Remembered
// bit. Remembered changes with MarkedYoung around GC_PHASE_ENUM.
constexpr bool is_store_bad(uintptr_t value, uintptr_t storeBadMask)
{
    return (value & storeBadMask) != 0;
}

// ZPointer::is_store_good -- load/mark-good plus the current Remembered epoch.
// Remembered flips with MarkedYoung at GC_PHASE_ENUM.
constexpr bool is_store_good(uintptr_t value, uintptr_t loadBadMask, uintptr_t storeBadMask)
{
    return is_load_good(value, loadBadMask) && !is_store_bad(value, storeBadMask) &&
        (value & current_marked_young(storeBadMask)) != 0 && (value & current_marked_old(storeBadMask)) != 0 &&
        (value & current_remembered(storeBadMask)) != 0;
}

constexpr bool is_store_good_or_null(uintptr_t value, uintptr_t loadBadMask, uintptr_t storeBadMask)
{
    return value == 0 || is_store_good(value, loadBadMask, storeBadMask);
}

// ZPointer::is_store_good_or_null above is true only for raw null or current
// remap + mark + Remembered bits. Stores in every GC_PHASE use this state; the
// relevant epochs change at ENUM and PREFORWARD as recorded above.

// ZPointer::is_marked_finalizable -- tests the current reserved finalizable
// epoch. Synthetic words can exercise it, but kFinalizableWired documents that
// no GC_PHASE currently publishes such a word.
constexpr bool is_marked_finalizable(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t finalizable = current_finalizable(markBadMask);
    return finalizable != 0 && (value & finalizable) != 0;
}

// ZPointer::is_marked_old -- true for the current MarkedOld epoch bit. Full
// collection flips it before GC_PHASE_ENUM; young collection leaves it alone.
constexpr bool is_marked_old(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t markedOld = current_marked_old(markBadMask);
    return markedOld != 0 && (value & markedOld) != 0;
}

// ZPointer::is_marked_young -- true for the current MarkedYoung epoch bit. It
// flips around GC_PHASE_ENUM in both full and young collections.
constexpr bool is_marked_young(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t markedYoung = current_marked_young(markBadMask);
    return markedYoung != 0 && (value & markedYoung) != 0;
}

// ZPointer::is_marked_any_old -- true for current MarkedOld or current
// Finalizable. Today only MarkedOld is publishable; Finalizable maps to no
// GC_PHASE while kFinalizableWired is false.
constexpr bool is_marked_any_old(uintptr_t value, uintptr_t markBadMask)
{
    return is_marked_old(value, markBadMask) || is_marked_finalizable(value, markBadMask);
}

// ZPointer::is_remembered_exact -- true when the current Remembered epoch bit
// is present. It changes with MarkedYoung around GC_PHASE_ENUM.
constexpr bool is_remembered_exact(uintptr_t value, uintptr_t storeBadMask)
{
    const uintptr_t remembered = current_remembered(storeBadMask);
    return remembered != 0 && (value & remembered) == remembered;
}

constexpr unsigned ZGC_PREDICATE_COUNT = 17u;

} // namespace ColourPredicates
} // namespace MapleRuntime

#endif // MRT_COLOUR_PREDICATES_H
