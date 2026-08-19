// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAL_COVERAGE_H
#define MRT_HEAL_COVERAGE_H

#include "Common/ColourMask.h"
#include "Common/ColourPredicates.h"

#include <cstddef>
#include <cstdint>

// Heal coverage before colour reuse.
//
// OpenJDK ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1508) plus the
// load barrier (zBarrier.inline.hpp:319-342) keep the invariant that a colour
// published at beat N is not still sitting in a live slot when beat N+2 reuses
// it (WCollector.h:175-180, zAddress.hpp:108-128: remap xor wraps every two
// relocate-start flips of that generation).
//
// This header is the classifier. The product walk lives in HealCoverage.cpp and
// is compiled out unless MRT_HEAL_COVERAGE_CENSUS=1 (default 0: rec=stw must
// not pay a full-heap walk). gc_unit drives the classifier directly.

#ifndef MRT_HEAL_COVERAGE_CENSUS
#define MRT_HEAL_COVERAGE_CENSUS 0
#endif

namespace MapleRuntime {
namespace HealCoverage {

constexpr bool kHealCoverageCensus = MRT_HEAL_COVERAGE_CENSUS != 0;

// A HeapSlot is load-good, plain (no remap bits: RootSlot ABI or null), or a
// coverage miss: it still carries a colour the current epoch does not hand out.
enum class Kind : uint8_t {
    Null = 0,
    Plain,
    LoadGood,
    Stale, // coloured, not load-good — the heal miss the census counts
};

constexpr Kind Classify(uintptr_t value, uintptr_t loadBadMask)
{
    if (value == 0) {
        return Kind::Null;
    }
    if ((value & REMAP_COLOUR_MASK) == 0) {
        return Kind::Plain;
    }
    if (ColourPredicates::is_load_good(value, loadBadMask)) {
        return Kind::LoadGood;
    }
    return Kind::Stale;
}

constexpr bool IsCoverageMiss(Kind k) { return k == Kind::Stale; }

// Face that was supposed to heal a stale slot (LEAD tagwide2 §2.1).
enum class Face : uint8_t {
    Unknown = 0,
    YoungHolder,  // young mark / FixMinorObjectSlots
    OldToYoung,   // remset / ScanPrevious / RemapYoungRoots
    OldToOld,     // old mark / InvalidateOldTaggedRefs
    PinnedHolder, // RecordPinned / FYS
    FromHolder,   // evacuate / FixMinor of from-space
};

struct Counts {
    size_t nulls = 0;
    size_t plains = 0;
    size_t loadGood = 0;
    size_t stale = 0;
    size_t youngHolder = 0;
    size_t oldToYoung = 0;
    size_t oldToOld = 0;
    size_t pinnedHolder = 0;
    size_t fromHolder = 0;
    size_t unknownFace = 0;
    size_t injectHits = 0;
};

inline void Add(Counts& c, Kind k)
{
    switch (k) {
        case Kind::Null:
            ++c.nulls;
            break;
        case Kind::Plain:
            ++c.plains;
            break;
        case Kind::LoadGood:
            ++c.loadGood;
            break;
        case Kind::Stale:
            ++c.stale;
            break;
    }
}

inline void AddFace(Counts& c, Face f)
{
    switch (f) {
        case Face::YoungHolder:
            ++c.youngHolder;
            break;
        case Face::OldToYoung:
            ++c.oldToYoung;
            break;
        case Face::OldToOld:
            ++c.oldToOld;
            break;
        case Face::PinnedHolder:
            ++c.pinnedHolder;
            break;
        case Face::FromHolder:
            ++c.fromHolder;
            break;
        case Face::Unknown:
            ++c.unknownFace;
            break;
    }
}

constexpr Face FaceOf(bool holderYoung, bool holderPinned, bool holderFrom, bool targetYoung)
{
    if (holderFrom) {
        return Face::FromHolder;
    }
    if (holderPinned) {
        return Face::PinnedHolder;
    }
    if (holderYoung) {
        return Face::YoungHolder;
    }
    if (targetYoung) {
        return Face::OldToYoung;
    }
    return Face::OldToOld;
}

// Synthetic census used by gc_unit and by the inject positive control. Does not
// walk the heap; the caller supplies the slot words.
inline Counts CensusWords(const uintptr_t* words, size_t n, uintptr_t loadBadMask)
{
    Counts c{};
    for (size_t i = 0; i < n; ++i) {
        Add(c, Classify(words[i], loadBadMask));
    }
    return c;
}

// Paint `addr` with the remap one-hot that was current *before* a young
// relocate-start xor. Used to inject a stale slot at the current epoch.
constexpr uintptr_t PreviousYoungRemap(uintptr_t currentRemap)
{
    // Young xor of REMAP_COLOUR_MASK swaps the accepted pair; the previous
    // combined one-hot is current XOR the two young-pair bits that moved.
    // Young mask is {00,10} or {01,11}; xor REMAP_COLOUR_MASK flips it.
    // Previous combined = current with the young half flipped, old half kept:
    //   current = youngMask & oldMask
    //   prev    = (youngMask ^ REMAP) & oldMask
    // which is current with its one-hot moved to the other young-accepted bit
    // that shares the same old half.
    if (currentRemap == ZPointerRemapped00) {
        return ZPointerRemapped01; // young flip 0101→1010, old 0011 kept
    }
    if (currentRemap == ZPointerRemapped01) {
        return ZPointerRemapped00;
    }
    if (currentRemap == ZPointerRemapped10) {
        return ZPointerRemapped11;
    }
    if (currentRemap == ZPointerRemapped11) {
        return ZPointerRemapped10;
    }
    return currentRemap;
}

constexpr uintptr_t PaintStale(uintptr_t addr, uintptr_t currentRemap)
{
    return (addr & ColourPredicates::HEAP_ADDRESS_MASK) | PreviousYoungRemap(currentRemap);
}

inline void InjectStaleOnce(uintptr_t* slot, uintptr_t currentRemap)
{
    if (slot == nullptr) {
        return;
    }
    const uintptr_t addr = *slot & ColourPredicates::HEAP_ADDRESS_MASK;
    *slot = PaintStale(addr, currentRemap);
}

void CensusAfterPublication(uintptr_t currentRemap, uint64_t flipSeq);

} // namespace HealCoverage
} // namespace MapleRuntime

#endif
