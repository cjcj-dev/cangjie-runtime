// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PHASE_COLOUR_CONTRACT_H
#define MRT_PHASE_COLOUR_CONTRACT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Input contract for replacing GCPhase-dependent decisions with the pure
// predicates in Common/ColourPredicates.h.
//
// Epoch bits alternate, so a phase can never mean a fixed *_0 or *_1 value.
// Each row instead says what happens to the current epoch while that phase is
// entered. UNCHANGED means "keep whichever epoch is current", not "epoch 0".
// The timing distinctions are deliberate: publishing a GCPhase and flipping a
// colour are separate writes today, so some phase names do not select one
// colour state for their whole lifetime.
//
// GC_PHASE_INIT is declared by Collector but has no WCollector transition or
// colour producer today. Both tables therefore mark every live family in its
// INIT row UNMAPPED instead of assigning it IDLE's state by guesswork.
enum class ColourCycleKind : uint8_t {
    FULL,
    YOUNG,
};

// Symbolic row keys only. They deliberately do not copy GCPhase's numeric
// values and must never be cast to/from GCPhase; a consumer switches its real
// GCPhase to the corresponding named row. Keeping this header below
// Collector.h avoids pulling the collector include graph into pure predicate
// users and avoids creating a second numeric phase truth.
enum class PhaseColourId : uint8_t {
    IDLE,
    INIT,
    ENUM,
    TRACE,
    CLEAR_SATB,
    POST_TRACE,
    PREFORWARD,
    FORWARD,
};

enum class ColourEpochTransition : uint8_t {
    UNCHANGED,
    FLIP_BEFORE_ENTER,
    FLIP_BEFORE_OR_DURING_PHASE,
    FLIP_DURING_PHASE,
    OPTIONAL_FLIP_DURING_PHASE_DEFAULT_OFF,
    UNMAPPED,
    UNPUBLISHED,
};

struct PhaseColourState {
    PhaseColourId phase;
    ColourEpochTransition remappedYoung;
    ColourEpochTransition remappedOld;
    ColourEpochTransition markedYoung;
    ColourEpochTransition markedOld;
    ColourEpochTransition remembered;
    ColourEpochTransition finalizable;
};

// Full collection:
//   * MarkedYoung/MarkedOld/Remembered flip before GC_PHASE_ENUM is published.
//   * RemappedYoung/RemappedOld flip after GC_PHASE_PREFORWARD has been
//     published by ScopedLightSync. PREFORWARD therefore has both pre- and
//     post-flip states today.
//   * Finalizable has reserved bits but kFinalizableWired is false, so no row
//     claims that a full collection publishes it.
constexpr PhaseColourState FULL_PHASE_COLOUR_MAP[] = {
    { PhaseColourId::IDLE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::INIT,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::ENUM,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::FLIP_BEFORE_ENTER,
      ColourEpochTransition::FLIP_BEFORE_ENTER,
      ColourEpochTransition::FLIP_BEFORE_ENTER,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::TRACE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::CLEAR_SATB,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::POST_TRACE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::PREFORWARD,
      ColourEpochTransition::FLIP_DURING_PHASE,
      ColourEpochTransition::FLIP_DURING_PHASE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::FORWARD,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
};

// Young collection:
//   * With concurrent stack scan, MarkedYoung/Remembered flip before ENUM is
//     published; without it, ScopedStopTheWorld publishes ENUM first and the
//     same flip happens inside ENUM. The phase alone cannot distinguish them.
//   * The young remap flip happens inside PREFORWARD only when
//     MRT_GCV2_MINOR_YOUNG_FLIP=1 or concurrent ref-fix implies it. Its product
//     default is off, so this table must not claim a mandatory post-flip state.
//   * MarkedOld/RemappedOld do not flip in a young-only cycle.
constexpr PhaseColourState YOUNG_PHASE_COLOUR_MAP[] = {
    { PhaseColourId::IDLE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::INIT,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNMAPPED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::ENUM,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::FLIP_BEFORE_OR_DURING_PHASE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::FLIP_BEFORE_OR_DURING_PHASE,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::TRACE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::CLEAR_SATB,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::POST_TRACE,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::PREFORWARD,
      ColourEpochTransition::OPTIONAL_FLIP_DURING_PHASE_DEFAULT_OFF,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
    { PhaseColourId::FORWARD,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNCHANGED,
      ColourEpochTransition::UNPUBLISHED },
};

constexpr size_t PHASE_COLOUR_ROW_COUNT = 8u;
static_assert(sizeof(FULL_PHASE_COLOUR_MAP) / sizeof(FULL_PHASE_COLOUR_MAP[0]) == PHASE_COLOUR_ROW_COUNT,
              "full phase-colour map must cover all eight contracted GC phases");
static_assert(sizeof(YOUNG_PHASE_COLOUR_MAP) / sizeof(YOUNG_PHASE_COLOUR_MAP[0]) == PHASE_COLOUR_ROW_COUNT,
              "young phase-colour map must cover all eight contracted GC phases");

constexpr const PhaseColourState* PhaseColourMap(ColourCycleKind cycle)
{
    return cycle == ColourCycleKind::FULL ? FULL_PHASE_COLOUR_MAP : YOUNG_PHASE_COLOUR_MAP;
}

} // namespace MapleRuntime

#endif // MRT_PHASE_COLOUR_CONTRACT_H
