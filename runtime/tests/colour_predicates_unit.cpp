// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Standalone C++14 unit for the ZGC-shaped colour predicate family. The
// default arm requires the product headers and gives every public predicate a
// positive and a negative word. MRT_COLOUR_PREDICATES_ALLOW_ABSENT exists only
// for the external removal arm: after the fix files are removed from a
// throw-away source copy, this same frozen test source reports predicates=0.

#if defined(MRT_COLOUR_PREDICATES_ALLOW_ABSENT)
#if defined(__has_include)
#if __has_include("Common/ColourPredicates.h") && __has_include("Heap/Collector/PhaseColourContract.h")
#define MRT_COLOUR_PREDICATES_PRESENT 1
#else
#define MRT_COLOUR_PREDICATES_PRESENT 0
#endif
#else
#error "MRT_COLOUR_PREDICATES_ALLOW_ABSENT requires __has_include"
#endif
#else
#define MRT_COLOUR_PREDICATES_PRESENT 1
#endif

#include <cstdio>

#if MRT_COLOUR_PREDICATES_PRESENT

#include "Common/ColourPredicates.h"
#include "Heap/Collector/PhaseColourContract.h"

namespace {
using namespace MapleRuntime;
using namespace MapleRuntime::ColourPredicates;

constexpr uintptr_t PAYLOAD = uintptr_t(0x1000);
constexpr EpochColours EPOCH = kInitialEpochColours;
constexpr BadMasks MASKS = ComputeBadMasks(EPOCH);

constexpr uintptr_t LOAD_GOOD = PAYLOAD | ZPointerRemapped00;
constexpr uintptr_t MARK_GOOD = LOAD_GOOD | MARKED_YOUNG_0 | MARKED_OLD_0;
constexpr uintptr_t STORE_GOOD = MARK_GOOD | REMEMBERED_0;
constexpr uintptr_t STALE_REMAP = PAYLOAD | ZPointerRemapped01;
constexpr uintptr_t OLD_HALF_BAD = PAYLOAD | ZPointerRemapped10;
constexpr uintptr_t STALE_MARK = LOAD_GOOD | MARKED_YOUNG_1 | MARKED_OLD_0;
constexpr uintptr_t STALE_REMEMBERED = MARK_GOOD | REMEMBERED_1;
constexpr uintptr_t MISSING_REMEMBERED = MARK_GOOD;

// 1. is_load_bad
static_assert(is_load_bad(STALE_REMAP, MASKS.loadBad), "COLOURPRED_POS_is_load_bad");
static_assert(!is_load_bad(LOAD_GOOD, MASKS.loadBad), "COLOURPRED_NEG_is_load_bad");

// 2. is_load_good
static_assert(is_load_good(LOAD_GOOD, MASKS.loadBad), "COLOURPRED_POS_is_load_good");
static_assert(!is_load_good(PAYLOAD, MASKS.loadBad), "COLOURPRED_NEG_is_load_good_plain");

// 3. is_load_good_or_null
static_assert(is_load_good_or_null(0, MASKS.loadBad), "COLOURPRED_POS_is_load_good_or_null");
static_assert(!is_load_good_or_null(PAYLOAD, MASKS.loadBad), "COLOURPRED_NEG_is_load_good_or_null_plain");

// 4. is_old_load_good
static_assert(is_old_load_good(LOAD_GOOD, MASKS.loadBad), "COLOURPRED_POS_is_old_load_good");
static_assert(!is_old_load_good(OLD_HALF_BAD, MASKS.loadBad), "COLOURPRED_NEG_is_old_load_good");

// 5. is_young_load_good
static_assert(is_young_load_good(LOAD_GOOD, MASKS.loadBad), "COLOURPRED_POS_is_young_load_good");
static_assert(!is_young_load_good(STALE_REMAP, MASKS.loadBad), "COLOURPRED_NEG_is_young_load_good");

// 6. is_mark_bad
static_assert(is_mark_bad(STALE_MARK, MASKS.markBad), "COLOURPRED_POS_is_mark_bad");
static_assert(!is_mark_bad(MARK_GOOD, MASKS.markBad), "COLOURPRED_NEG_is_mark_bad");

// 7. is_mark_good
static_assert(is_mark_good(MARK_GOOD, MASKS.loadBad, MASKS.markBad), "COLOURPRED_POS_is_mark_good");
static_assert(!is_mark_good(STALE_MARK, MASKS.loadBad, MASKS.markBad), "COLOURPRED_NEG_is_mark_good");

// 8. is_mark_good_or_null
static_assert(is_mark_good_or_null(0, MASKS.loadBad, MASKS.markBad), "COLOURPRED_POS_is_mark_good_or_null");
static_assert(!is_mark_good_or_null(PAYLOAD, MASKS.loadBad, MASKS.markBad),
              "COLOURPRED_NEG_is_mark_good_or_null_plain");

// 9. is_store_bad
static_assert(is_store_bad(STALE_REMEMBERED, MASKS.storeBad), "COLOURPRED_POS_is_store_bad");
static_assert(!is_store_bad(STORE_GOOD, MASKS.storeBad), "COLOURPRED_NEG_is_store_bad");

// 10. is_store_good
static_assert(is_store_good(STORE_GOOD, MASKS.loadBad, MASKS.storeBad), "COLOURPRED_POS_is_store_good");
static_assert(!is_store_good(MISSING_REMEMBERED, MASKS.loadBad, MASKS.storeBad),
              "COLOURPRED_NEG_is_store_good_missing_remembered");

// 11. is_store_good_or_null
static_assert(is_store_good_or_null(0, MASKS.loadBad, MASKS.storeBad),
              "COLOURPRED_POS_is_store_good_or_null");
static_assert(!is_store_good_or_null(MISSING_REMEMBERED, MASKS.loadBad, MASKS.storeBad),
              "COLOURPRED_NEG_is_store_good_or_null_missing_remembered");

// 12. is_marked_finalizable
static_assert(is_marked_finalizable(PAYLOAD | FINALIZABLE_0, MASKS.markBad),
              "COLOURPRED_POS_is_marked_finalizable");
static_assert(!is_marked_finalizable(PAYLOAD | FINALIZABLE_1, MASKS.markBad),
              "COLOURPRED_NEG_is_marked_finalizable");

// 13. is_marked_old
static_assert(is_marked_old(PAYLOAD | MARKED_OLD_0, MASKS.markBad), "COLOURPRED_POS_is_marked_old");
static_assert(!is_marked_old(PAYLOAD | MARKED_OLD_1, MASKS.markBad), "COLOURPRED_NEG_is_marked_old");

// 14. is_marked_young
static_assert(is_marked_young(PAYLOAD | MARKED_YOUNG_0, MASKS.markBad), "COLOURPRED_POS_is_marked_young");
static_assert(!is_marked_young(PAYLOAD | MARKED_YOUNG_1, MASKS.markBad), "COLOURPRED_NEG_is_marked_young");

// 15. is_marked_any_old
static_assert(is_marked_any_old(PAYLOAD | FINALIZABLE_0, MASKS.markBad),
              "COLOURPRED_POS_is_marked_any_old");
static_assert(!is_marked_any_old(PAYLOAD | MARKED_OLD_1 | FINALIZABLE_1, MASKS.markBad),
              "COLOURPRED_NEG_is_marked_any_old");

// 16. is_remapped
static_assert(is_remapped(LOAD_GOOD, MASKS.loadBad), "COLOURPRED_POS_is_remapped");
static_assert(!is_remapped(STALE_REMAP, MASKS.loadBad), "COLOURPRED_NEG_is_remapped");

// 17. is_remembered_exact
static_assert(is_remembered_exact(PAYLOAD | REMEMBERED_0, MASKS.storeBad),
              "COLOURPRED_POS_is_remembered_exact");
static_assert(!is_remembered_exact(PAYLOAD | REMEMBERED_1, MASKS.storeBad),
              "COLOURPRED_NEG_is_remembered_exact");

constexpr EpochColours EpochAt(unsigned i)
{
    return EpochColours{
        (i & 1u) ? (ZPointerRemapped01 | ZPointerRemapped11) : (ZPointerRemapped10 | ZPointerRemapped00),
        (i & 2u) ? (ZPointerRemapped10 | ZPointerRemapped11) : (ZPointerRemapped01 | ZPointerRemapped00),
        (i & 4u) ? MARKED_YOUNG_1 : MARKED_YOUNG_0,
        (i & 8u) ? MARKED_OLD_1 : MARKED_OLD_0,
        (i & 16u) ? REMEMBERED_1 : REMEMBERED_0
    };
}

// The predicates derive current bits from ComputeBadMasks on the entire
// 2^5 epoch cube. This is the mechanical no-second-truth check.
constexpr bool PublishedMasksAreTheOnlyTruth()
{
    bool ok = true;
    for (unsigned i = 0; i < 32u; ++i) {
        const EpochColours epoch = EpochAt(i);
        const BadMasks masks = ComputeBadMasks(epoch);
        const uintptr_t expectedFinalizable =
            epoch.markedOld == MARKED_OLD_0 ? FINALIZABLE_0 : FINALIZABLE_1;
        if (current_remapped(masks.loadBad) != masks.remapColour ||
            current_marked_young(masks.markBad) != epoch.markedYoung ||
            current_marked_old(masks.markBad) != epoch.markedOld ||
            current_finalizable(masks.markBad) != expectedFinalizable ||
            current_remembered(masks.storeBad) != epoch.remembered) {
            ok = false;
        }
    }
    return ok;
}

static_assert(PublishedMasksAreTheOnlyTruth(), "COLOURPRED_PUBLISHED_MASK_TRUTH_32_EPOCHS");
static_assert(ZGC_PREDICATE_COUNT == 17u, "COLOURPRED_ZGC_NAME_COUNT");
static_assert(PHASE_COLOUR_ROW_COUNT == 8u, "COLOURPRED_PHASE_ROW_COUNT");

// Pin the phase cells that cannot honestly be represented as a single state.
static_assert(FULL_PHASE_COLOUR_MAP[1].phase == PhaseColourId::INIT &&
                  FULL_PHASE_COLOUR_MAP[1].remappedYoung == ColourEpochTransition::UNMAPPED,
              "COLOURPRED_FULL_INIT_UNMAPPED");
static_assert(FULL_PHASE_COLOUR_MAP[6].phase == PhaseColourId::PREFORWARD &&
                  FULL_PHASE_COLOUR_MAP[6].remappedYoung == ColourEpochTransition::FLIP_DURING_PHASE &&
                  FULL_PHASE_COLOUR_MAP[6].remappedOld == ColourEpochTransition::FLIP_DURING_PHASE,
              "COLOURPRED_FULL_PREFORWARD_HAS_TWO_STATES");
static_assert(YOUNG_PHASE_COLOUR_MAP[2].phase == PhaseColourId::ENUM &&
                  YOUNG_PHASE_COLOUR_MAP[2].markedYoung ==
                      ColourEpochTransition::FLIP_BEFORE_OR_DURING_PHASE,
              "COLOURPRED_YOUNG_ENUM_TIMING_DEPENDS_ON_STACK_SCAN");
static_assert(YOUNG_PHASE_COLOUR_MAP[6].phase == PhaseColourId::PREFORWARD &&
                  YOUNG_PHASE_COLOUR_MAP[6].remappedYoung ==
                      ColourEpochTransition::OPTIONAL_FLIP_DURING_PHASE_DEFAULT_OFF,
              "COLOURPRED_YOUNG_PREFORWARD_OPTIONAL_DEFAULT_OFF");
static_assert(!kFinalizableWired, "COLOURPRED_FINALIZABLE_MUST_REMAIN_UNPUBLISHED");

} // namespace

int main()
{
    std::printf("COLOUR_PREDICATES predicates=%u positive_vectors=17 negative_vectors=17 "
                "phase_rows=%zu mask_epochs=32 finalizable_wired=%u\n",
                ZGC_PREDICATE_COUNT, 2u * PHASE_COLOUR_ROW_COUNT, static_cast<unsigned>(kFinalizableWired));
    return 0;
}

#else

int main()
{
    std::printf("COLOUR_PREDICATES predicates=0 positive_vectors=0 negative_vectors=0 "
                "phase_rows=0 mask_epochs=0 finalizable_wired=0\n");
    return 0;
}

#endif
