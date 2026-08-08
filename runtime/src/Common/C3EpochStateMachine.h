// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_C3_EPOCH_STATE_MACHINE_H
#define MRT_C3_EPOCH_STATE_MACHINE_H

// C3 young/old epoch + store-good state machine (design-only skeleton).
// Design truth: ops/design/C3_EPOCH_STATE_MACHINE.md
// OpenJDK jdk-25-ga anchors: zAddress.hpp:48-176, zAddress.cpp:78-152,
// zAddress.inline.hpp:627-726, zBarrier.inline.hpp:110-137,695-755.
//
// Not wired into the live collector loop. Live g_cjLoadBadMask / g_cjMarkBadMask
// keep today's formula until C4.

#include <cstdint>

#include "Common/ColourMask.h"

namespace MapleRuntime {
namespace C3 {

// Planned bit placement in today's RefField padding (ColourMask.h:63-65).
// address:48 + isTagged:1 + tagID:TAG_ID_BITS + remap:4 + MY:2 + MO:2 + Rem:2 + Fin:2 + spare:2
constexpr unsigned REMEMBERED_BITS = 2u;
constexpr unsigned FINALIZABLE_BITS = 2u;
constexpr unsigned REMEMBERED_SHIFT = MARKED_OLD_SHIFT + MARKED_OLD_BITS;
constexpr unsigned FINALIZABLE_SHIFT = REMEMBERED_SHIFT + REMEMBERED_BITS;
constexpr uintptr_t REMEMBERED_0 = uintptr_t(1) << REMEMBERED_SHIFT;
constexpr uintptr_t REMEMBERED_1 = uintptr_t(1) << (REMEMBERED_SHIFT + 1u);
constexpr uintptr_t REMEMBERED_MASK = REMEMBERED_0 | REMEMBERED_1;
constexpr uintptr_t FINALIZABLE_0 = uintptr_t(1) << FINALIZABLE_SHIFT;
constexpr uintptr_t FINALIZABLE_1 = uintptr_t(1) << (FINALIZABLE_SHIFT + 1u);
constexpr uintptr_t FINALIZABLE_MASK = FINALIZABLE_0 | FINALIZABLE_1;
// Remaining padding after C3 families (must stay non-negative).
constexpr unsigned C3_SPARE_PADDING_BITS =
    TAG_ID_PADDING_BITS > (REMEMBERED_BITS + FINALIZABLE_BITS)
        ? (TAG_ID_PADDING_BITS - REMEMBERED_BITS - FINALIZABLE_BITS)
        : 0u;
static_assert(REMEMBERED_BITS + FINALIZABLE_BITS <= TAG_ID_PADDING_BITS,
              "C3 Remembered+Finalizable need free RefField padding bits");

// Drop-bit modes for positive-control model checks (task §3 ①).
enum class DropBit : uint8_t {
    None = 0,
    Remembered,
    Finalizable,
    MarkedYoung,
    MarkedOld,
    RemapSplit, // collapse young/old remap distinction into one bit family
};

// Global epoch view after a sequence of mark/relocate flips.
// Mirrors ZGlobalsPointers fields (zAddress.cpp:120-127).
struct EpochView {
    uint8_t youngRemapEpoch = 0; // conceptual RemappedYoung[0|1]
    uint8_t oldRemapEpoch = 0;   // conceptual RemappedOld[0|1]
    uint8_t markedYoung = 0;    // index of current MarkedYoung bit
    uint8_t markedOld = 0;
    uint8_t remembered = 0;     // index of current Remembered good bit
    uint8_t finalizable = 0;
};

// Well-formed non-null pointer colour (metadata only; address bits ignored).
struct PointerColour {
    uint8_t remapCode = 0; // 0..3 → Remapped00..11 one-hot
    uint8_t markedYoung = 0;
    uint8_t markedOld = 0;
    uint8_t remembered = 0;   // 0=bit0, 1=bit1, 2=both (forgotten)
    uint8_t finalizable = 0;
    bool isTagged = false;
    bool isNull = false;
    bool plainNonNull = false; // ill-formed: metadata all zero, non-null
};

struct BarrierJudgement {
    bool loadGood = false;
    bool markGood = false;
    bool storeGood = false;
    bool finalizableMarkGood = false;
    bool wellFormed = false;
    bool uncertain = false; // true only under DropBit model checks
};

struct GoodMasks {
    uintptr_t remapped = 0;
    uintptr_t loadGood = 0;
    uintptr_t markGood = 0;
    uintptr_t storeGood = 0;
    uintptr_t loadBad = 0;
    uintptr_t markBad = 0;
    uintptr_t storeBad = 0;
    uintptr_t remappedYoungMask = 0;
    uintptr_t remappedOldMask = 0;
    uintptr_t markedYoungBit = 0;
    uintptr_t markedOldBit = 0;
    uintptr_t rememberedBit = 0;
    uintptr_t finalizableBit = 0;
};

// RemappedYoungMask / RemappedOldMask from conceptual epochs
// (zAddress.hpp:113-128, zAddress.cpp:120-121).
inline uintptr_t remapped_young_mask(uint8_t youngEpoch)
{
    // Young0 => 0101 bits of the four; Young1 => 1010. Encoded as the two accepted one-hots.
    return (youngEpoch & 1u) ? (ZPointerRemapped01 | ZPointerRemapped11)
                             : (ZPointerRemapped00 | ZPointerRemapped10);
}

inline uintptr_t remapped_old_mask(uint8_t oldEpoch)
{
    // Old0 => 0011; Old1 => 1100.
    return (oldEpoch & 1u) ? (ZPointerRemapped10 | ZPointerRemapped11)
                           : (ZPointerRemapped00 | ZPointerRemapped01);
}

inline uintptr_t remap_one_hot(uint8_t remapCode)
{
    return uintptr_t(1) << (REMAP_COLOUR_SHIFT + (remapCode & 3u));
}

inline uintptr_t marked_young_bit(uint8_t idx)
{
    return (idx & 1u) ? MARKED_YOUNG_1 : MARKED_YOUNG_0;
}

inline uintptr_t marked_old_bit(uint8_t idx)
{
    return (idx & 1u) ? MARKED_OLD_1 : MARKED_OLD_0;
}

inline uintptr_t remembered_bits(uint8_t code)
{
    if (code == 2u) {
        return REMEMBERED_MASK;
    }
    return (code & 1u) ? REMEMBERED_1 : REMEMBERED_0;
}

inline uintptr_t finalizable_bit(uint8_t idx)
{
    return (idx & 1u) ? FINALIZABLE_1 : FINALIZABLE_0;
}

// zAddress.cpp:78-87 set_good_masks, extended with store family.
inline GoodMasks set_good_masks(const EpochView& e, DropBit drop = DropBit::None)
{
    GoodMasks m {};
    m.remappedYoungMask = remapped_young_mask(e.youngRemapEpoch);
    m.remappedOldMask = remapped_old_mask(e.oldRemapEpoch);
    if (drop == DropBit::RemapSplit) {
        // Collapse young/old into one accepted remap bit (loses generation split).
        m.remappedYoungMask = (e.youngRemapEpoch & 1u) ? ZPointerRemapped11 : ZPointerRemapped00;
        m.remappedOldMask = m.remappedYoungMask;
    }
    m.remapped = m.remappedYoungMask & m.remappedOldMask;
    m.markedYoungBit = marked_young_bit(e.markedYoung);
    m.markedOldBit = marked_old_bit(e.markedOld);
    m.rememberedBit = remembered_bits(e.remembered & 1u);
    m.finalizableBit = finalizable_bit(e.finalizable);

    m.loadGood = m.remapped;
    m.markGood = m.loadGood | m.markedYoungBit | m.markedOldBit;
    if (drop == DropBit::MarkedYoung) {
        m.markGood = m.loadGood | m.markedOldBit;
    }
    if (drop == DropBit::MarkedOld) {
        m.markGood = m.loadGood | m.markedYoungBit;
    }
    m.storeGood = m.markGood | m.rememberedBit;
    if (drop == DropBit::Remembered) {
        m.storeGood = m.markGood; // store collapses to mark — positive control
    }

    const uintptr_t loadMeta = REMAP_COLOUR_MASK;
    const uintptr_t markMeta = loadMeta | MARKED_YOUNG_MASK | MARKED_OLD_MASK | FINALIZABLE_MASK;
    const uintptr_t storeMeta = markMeta | REMEMBERED_MASK;
    m.loadBad = m.loadGood ^ loadMeta;
    m.markBad = m.markGood ^ markMeta;
    m.storeBad = m.storeGood ^ storeMeta;
    if (drop == DropBit::Finalizable) {
        // Without Finalizable family, markMeta loses Fin bits — resurrection unrepresentable.
        const uintptr_t markMetaNoFin = loadMeta | MARKED_YOUNG_MASK | MARKED_OLD_MASK;
        m.markBad = m.markGood ^ markMetaNoFin;
        m.storeBad = m.storeGood ^ (markMetaNoFin | REMEMBERED_MASK);
    }
    return m;
}

inline void flip_young_mark_start(EpochView& e)
{
    e.markedYoung ^= 1u;
    e.remembered ^= 1u;
}

inline void flip_young_relocate_start(EpochView& e)
{
    e.youngRemapEpoch ^= 1u;
}

inline void flip_old_mark_start(EpochView& e)
{
    e.markedOld ^= 1u;
    e.finalizable ^= 1u;
}

inline void flip_old_relocate_start(EpochView& e)
{
    e.oldRemapEpoch ^= 1u;
}

inline uintptr_t colour_word(const PointerColour& p)
{
    if (p.isNull) {
        return 0;
    }
    if (p.plainNonNull) {
        return 0x1000; // non-zero address bits only
    }
    uintptr_t w = remap_one_hot(p.remapCode) | marked_young_bit(p.markedYoung) | marked_old_bit(p.markedOld) |
        remembered_bits(p.remembered) | finalizable_bit(p.finalizable);
    if (p.isTagged) {
        w |= (uintptr_t(1) << 48);
    }
    return w;
}

inline bool is_well_formed(const PointerColour& p)
{
    if (p.isNull || p.plainNonNull) {
        return false;
    }
    if (p.remapCode > 3u || p.markedYoung > 1u || p.markedOld > 1u || p.finalizable > 1u) {
        return false;
    }
    if (p.remembered > 2u) {
        return false;
    }
    return true;
}

// Barrier predicates — every non-null well-formed colour has a definite good/bad.
inline BarrierJudgement judge(const PointerColour& p, const EpochView& e, DropBit drop = DropBit::None)
{
    BarrierJudgement j {};
    const GoodMasks m = set_good_masks(e, drop);
    if (p.isNull) {
        j.wellFormed = true; // null is a defined colour
        return j;            // all good flags false
    }
    if (p.plainNonNull) {
        j.wellFormed = false;
        // C3: ill-formed non-null is never good (no trust state).
        return j;
    }
    j.wellFormed = is_well_formed(p);
    if (!j.wellFormed) {
        return j;
    }

    const uintptr_t w = colour_word(p);
    const uintptr_t remapBits = w & REMAP_COLOUR_MASK;
    bool youngLoadGood = (remapBits & m.remappedYoungMask) != 0;
    bool oldLoadGood = (remapBits & m.remappedOldMask) != 0;

    if (drop == DropBit::RemapSplit) {
        // Cannot tell which generation is load-good when both masks are identical and
        // the pointer carries a bit accepted by neither or both ambiguously after flip.
        if (!youngLoadGood && !oldLoadGood) {
            j.uncertain = true;
        }
    }

    if (p.isTagged) {
        youngLoadGood = false;
        oldLoadGood = false;
    }

    j.loadGood = youngLoadGood && oldLoadGood;

    const bool myMatch = (drop == DropBit::MarkedYoung)
        ? true // bit deleted ⇒ "match" is undefined; mark good collapses
        : ((w & MARKED_YOUNG_MASK) == m.markedYoungBit);
    const bool moMatch = (drop == DropBit::MarkedOld) ? true : ((w & MARKED_OLD_MASK) == m.markedOldBit);

    if (drop == DropBit::MarkedYoung || drop == DropBit::MarkedOld) {
        // Without an independent epoch bit, concurrent young/old mark cannot decide
        // whether a pointer that is load-good still needs mark work for the dropped gen.
        if (j.loadGood) {
            j.uncertain = true;
        }
    }

    j.markGood = j.loadGood && myMatch && moMatch;

    const bool remExact = (drop == DropBit::Remembered)
        ? true
        : ((w & REMEMBERED_MASK) == m.rememberedBit);
    j.storeGood = j.markGood && remExact;

    if (drop == DropBit::Remembered) {
        // Double-remap-bad generation choice loses the BOTH→old rule.
        if (!youngLoadGood && !oldLoadGood) {
            j.uncertain = true;
        }
    }

    const bool finMatch = (drop == DropBit::Finalizable) ? false : ((w & FINALIZABLE_MASK) == m.finalizableBit);
    j.finalizableMarkGood = j.loadGood && myMatch && finMatch && !moMatch;

    if (drop == DropBit::Finalizable) {
        // Cannot distinguish finalizable-only from unmarked for strong upgrade.
        if (j.loadGood && myMatch && !moMatch) {
            j.uncertain = true;
        }
    }

    return j;
}

// zBarrier.inline.hpp:110-137 remap_generation protocol.
enum class RemapGeneration : uint8_t { Young, Old, Uncertain };

struct ForwardingPresence {
    bool inYoungTable = false;
    bool inOldTable = false;
};

inline RemapGeneration select_remap_generation(const PointerColour& p, const EpochView& e,
                                               ForwardingPresence tables, DropBit drop = DropBit::None)
{
    if (p.isNull || p.plainNonNull || !is_well_formed(p)) {
        return RemapGeneration::Uncertain;
    }
    const GoodMasks m = set_good_masks(e, drop);
    const uintptr_t remapBits = colour_word(p) & REMAP_COLOUR_MASK;
    const bool youngLoadGood = (remapBits & m.remappedYoungMask) != 0 && !p.isTagged;
    const bool oldLoadGood = (remapBits & m.remappedOldMask) != 0 && !p.isTagged;

    if (oldLoadGood && !youngLoadGood) {
        return RemapGeneration::Young;
    }
    if (youngLoadGood && !oldLoadGood) {
        return RemapGeneration::Old;
    }
    if (youngLoadGood && oldLoadGood) {
        return RemapGeneration::Uncertain; // load-good needs no remap
    }

    // Double remap-bad.
    if (drop == DropBit::Remembered || drop == DropBit::RemapSplit) {
        return RemapGeneration::Uncertain;
    }
    const uintptr_t rem = colour_word(p) & REMEMBERED_MASK;
    if (rem == REMEMBERED_MASK) {
        return RemapGeneration::Old; // old-to-old forgotten
    }
    if (tables.inYoungTable && !tables.inOldTable) {
        return RemapGeneration::Young;
    }
    if (tables.inOldTable && !tables.inYoungTable) {
        return RemapGeneration::Old;
    }
    if (tables.inYoungTable && tables.inOldTable) {
        return RemapGeneration::Uncertain; // violates mutual exclusion
    }
    return RemapGeneration::Old;
}

// Dual forwarding table skeleton (not connected to RegionInfo).
struct ForwardingTables {
    // Presence only for model checks; C7 installs real maps.
    bool (*youngContains)(uintptr_t addr) = nullptr;
    bool (*oldContains)(uintptr_t addr) = nullptr;

    ForwardingPresence probe(uintptr_t addr) const
    {
        ForwardingPresence t {};
        if (youngContains != nullptr) {
            t.inYoungTable = youngContains(addr);
        }
        if (oldContains != nullptr) {
            t.inOldTable = oldContains(addr);
        }
        return t;
    }
};

// Model-check counters for the transition table.
struct ModelStats {
    uint64_t cells = 0;
    uint64_t nonNullCells = 0;
    uint64_t uncertain = 0;
    uint64_t trustLike = 0; // plain treated as good — must stay 0
    uint64_t loadGood = 0;
    uint64_t markGood = 0;
    uint64_t storeGood = 0;
};

inline void accumulate_cell(ModelStats& s, const PointerColour& p, const BarrierJudgement& j)
{
    s.cells++;
    if (p.isNull) {
        if (j.loadGood || j.markGood || j.storeGood) {
            s.trustLike++;
        }
        return;
    }
    s.nonNullCells++;
    if (j.uncertain) {
        s.uncertain++;
    }
    if (p.plainNonNull && (j.loadGood || j.markGood || j.storeGood)) {
        s.trustLike++;
    }
    if (j.loadGood) {
        s.loadGood++;
    }
    if (j.markGood) {
        s.markGood++;
    }
    if (j.storeGood) {
        s.storeGood++;
    }
}

// Enumerate well-formed colours × a fixed epoch, plus ill-formed plains.
inline ModelStats enumerate_epoch(const EpochView& e, DropBit drop = DropBit::None)
{
    ModelStats s {};
    // null
    {
        PointerColour p {};
        p.isNull = true;
        accumulate_cell(s, p, judge(p, e, drop));
    }
    // plain non-null (must never be good)
    {
        PointerColour p {};
        p.plainNonNull = true;
        accumulate_cell(s, p, judge(p, e, drop));
    }
    for (uint8_t rc = 0; rc < 4; ++rc) {
        for (uint8_t my = 0; my < 2; ++my) {
            for (uint8_t mo = 0; mo < 2; ++mo) {
                for (uint8_t rem = 0; rem < 3; ++rem) {
                    for (uint8_t fin = 0; fin < 2; ++fin) {
                        for (int tag = 0; tag < 2; ++tag) {
                            PointerColour p {};
                            p.remapCode = rc;
                            p.markedYoung = my;
                            p.markedOld = mo;
                            p.remembered = rem;
                            p.finalizable = fin;
                            p.isTagged = tag != 0;
                            accumulate_cell(s, p, judge(p, e, drop));
                        }
                    }
                }
            }
        }
    }
    return s;
}

} // namespace C3
} // namespace MapleRuntime

#endif // MRT_C3_EPOCH_STATE_MACHINE_H
