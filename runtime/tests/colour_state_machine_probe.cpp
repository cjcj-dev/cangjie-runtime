// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Compile-time colour state machine table.
//
// Design truth: ops/design/C3_EPOCH_STATE_MACHINE.md. OpenJDK anchors are jdk-25-ga under
// /root/cj_build/reference/jdk (zAddress.hpp, zAddress.cpp, zAddress.inline.hpp,
// zBarrier.inline.hpp).
//
// Why this file exists, and why it is not the old skeleton
// -------------------------------------------------------
// The 08-12 rewrite removed runtime/src/Common/C3EpochStateMachine.h and its probe. Commit
// 9694d301 (on the pre-rewrite line; not an ancestor of main) says in so many words:
//   "store-good skeleton misleads; rebuild from ZGC ZPointerStoreGoodMask if needed -- do not
//    resurrect this file."
// That ruling has since been paid: the store family is live (ColourMask.h REMEMBERED_*,
// Collector.h:199-208 is_store_good/is_store_bad, WCollector.h g_cjStoreBadMask). What was
// carried off with the skeleton and never replaced is the *check*: the exhaustive table and its
// positive controls. So this probe rebuilds the check against the product's own formula
// (ColourMask.h::ComputeBadMasks) and adds no second copy of it to the source tree.
//
// The old skeleton also could not have proved what it claimed. Its `uncertain` flag was set by
// hand in four `if (drop == DropBit::X)` branches, so a green run only proved that somebody had
// written those four lines. Here "dropping a family creates an ambiguity" is *computed*: two
// colours that differ only in the dropped family are compared through the same action function,
// and a disagreement is a collision. There is no place to write the answer down.
//
// Nothing here is compiled into the runtime. runtime/CMakeLists.txt builds it with try_compile
// on every configure (control arm plus three inject arms); it has no main() unless
// MRT_C4TABLE_PRINT_WITNESS is defined, which the manual arm
// (runtime/tests/run_colour_table_witness.sh) uses to print the witness colours for a human.

#include "Common/ColourMask.h"

namespace {
using MapleRuntime::BadMasks;
using MapleRuntime::ComputeBadMasks;
using MapleRuntime::EpochColours;

using MapleRuntime::MARKED_OLD_0;
using MapleRuntime::MARKED_OLD_1;
using MapleRuntime::MARKED_OLD_MASK;
using MapleRuntime::MARKED_YOUNG_0;
using MapleRuntime::MARKED_YOUNG_1;
using MapleRuntime::MARKED_YOUNG_MASK;
using MapleRuntime::REMAP_COLOUR_MASK;
using MapleRuntime::REMAP_COLOUR_SHIFT;
using MapleRuntime::REMEMBERED_0;
using MapleRuntime::REMEMBERED_1;
using MapleRuntime::REMEMBERED_MASK;
using MapleRuntime::TAGGED_BITS_MASK;
using MapleRuntime::ZPointerRemapped00;
using MapleRuntime::ZPointerRemapped01;
using MapleRuntime::ZPointerRemapped10;
using MapleRuntime::ZPointerRemapped11;
using MapleRuntime::FINALIZABLE_0;
using MapleRuntime::FINALIZABLE_1;
using MapleRuntime::FINALIZABLE_MASK;

// ---------------------------------------------------------------------------
// Part 1 -- the published formula, all 32 epochs, product against a literal copy
// ---------------------------------------------------------------------------
// Five two-state words drive the masks (young remap epoch, old remap epoch, MarkedYoung,
// MarkedOld, Remembered), so 2^5 = 32 cells cover the formula exhaustively. Reachability is
// narrower than that -- Remembered flips with MarkedYoung (zAddress.cpp:132-136) -- but the
// formula has to hold on the whole cube, and the wider domain costs nothing.

constexpr EpochColours EpochAt(unsigned i)
{
    return EpochColours{
        // flip_young_relocate_start XORs REMAP_COLOUR_MASK over the initial (10|00).
        (i & 1u) ? (ZPointerRemapped01 | ZPointerRemapped11) : (ZPointerRemapped10 | ZPointerRemapped00),
        // flip_old_relocate_start, initial (01|00).
        (i & 2u) ? (ZPointerRemapped10 | ZPointerRemapped11) : (ZPointerRemapped01 | ZPointerRemapped00),
        (i & 4u) ? MARKED_YOUNG_1 : MARKED_YOUNG_0,
        (i & 8u) ? MARKED_OLD_1 : MARKED_OLD_0,
        (i & 16u) ? REMEMBERED_1 : REMEMBERED_0
    };
}

// Literal copy of WCollector::set_good_masks as it stood at 6adf9dd0. This is the *witness*: it
// must never be rewritten as a call to ComputeBadMasks, or the comparison compares one
// implementation with itself and reports agreement for ever.
constexpr BadMasks WitnessBadMasks(EpochColours e)
{
    return BadMasks{
        e.remappedYoungMask & e.remappedOldMask,
#ifdef MRT_C4TABLE_INJECT_FORMULA
        // Positive control: flip a remap bit so the witness disagrees with ComputeBadMasks.
        // (Used to drop TAGGED_BITS_MASK; that term is now 0, so dropping it is a no-op.)
        (REMAP_COLOUR_MASK ^ (e.remappedYoungMask & e.remappedOldMask)) ^ ZPointerRemapped00,
#else
        TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ (e.remappedYoungMask & e.remappedOldMask)),
#endif
        0, 0
    };
}

constexpr uintptr_t WitnessMarkBad(EpochColours e)
{
    return WitnessBadMasks(e).loadBad | (MARKED_YOUNG_MASK & ~e.markedYoung) |
        (MARKED_OLD_MASK & ~e.markedOld);
}

constexpr uintptr_t WitnessStoreBad(EpochColours e)
{
    return WitnessMarkBad(e) | (REMEMBERED_MASK & ~e.remembered);
}

constexpr bool MaskEquivAt(unsigned i)
{
    return ComputeBadMasks(EpochAt(i)).remapColour == WitnessBadMasks(EpochAt(i)).remapColour &&
        ComputeBadMasks(EpochAt(i)).loadBad == WitnessBadMasks(EpochAt(i)).loadBad &&
        ComputeBadMasks(EpochAt(i)).markBad == WitnessMarkBad(EpochAt(i)) &&
        ComputeBadMasks(EpochAt(i)).storeBad == WitnessStoreBad(EpochAt(i));
}

constexpr bool MaskEquivAllEpochs()
{
    bool ok = true;
    for (unsigned i = 0; i < 32u; ++i) {
        if (!MaskEquivAt(i)) {
            ok = false;
        }
    }
    return ok;
}

// load-bad ⊆ mark-bad ⊆ store-bad, i.e. store-good ⇒ mark-good ⇒ load-good, on every epoch.
// This is what makes the three barriers a lattice rather than three unrelated predicates.
constexpr bool LatticeOrderHolds()
{
    bool ok = true;
    for (unsigned i = 0; i < 32u; ++i) {
        const BadMasks m = ComputeBadMasks(EpochAt(i));
        if ((m.markBad & m.loadBad) != m.loadBad) {
            ok = false;
        }
        if ((m.storeBad & m.markBad) != m.markBad) {
            ok = false;
        }
    }
    return ok;
}

// Mid-evacuation is no longer a pointer bit (TAGGED_BITS_MASK == 0). The
// identity (0 & mask) == 0 holds on every epoch; keep the check so a
// regression that reintroduces a non-zero tagged term is a table failure.
constexpr bool TaggedImpliesAllBad()
{
    bool ok = true;
    for (unsigned i = 0; i < 32u; ++i) {
        const BadMasks m = ComputeBadMasks(EpochAt(i));
        if ((TAGGED_BITS_MASK & m.loadBad) != TAGGED_BITS_MASK) {
            ok = false;
        }
        if ((TAGGED_BITS_MASK & m.markBad) != TAGGED_BITS_MASK) {
            ok = false;
        }
        if ((TAGGED_BITS_MASK & m.storeBad) != TAGGED_BITS_MASK) {
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Part 2 -- the state machine: colours, epochs, events, actions
// ---------------------------------------------------------------------------

// Reachable global epoch. Four free bits: the two remap epochs flip independently
// (zAddress.cpp:138-151), Remembered flips with MarkedYoung (:132-136) and Finalizable flips
// with MarkedOld (:143-147), so those two pairs are one bit each.
struct ModelEpoch {
    unsigned yRemap;
    unsigned oRemap;
    unsigned yMark; // also the current Remembered epoch
    unsigned oMark; // also the current Finalizable epoch
};

constexpr unsigned kEpochCount = 16u;

constexpr ModelEpoch ModelEpochAt(unsigned i)
{
    return ModelEpoch{ i & 1u, (i >> 1) & 1u, (i >> 2) & 1u, (i >> 3) & 1u };
}

constexpr EpochColours EpochColoursOf(ModelEpoch e)
{
    return EpochColours{
        e.yRemap ? (ZPointerRemapped01 | ZPointerRemapped11) : (ZPointerRemapped10 | ZPointerRemapped00),
        e.oRemap ? (ZPointerRemapped10 | ZPointerRemapped11) : (ZPointerRemapped01 | ZPointerRemapped00),
        e.yMark ? MARKED_YOUNG_1 : MARKED_YOUNG_0,
        e.oMark ? MARKED_OLD_1 : MARKED_OLD_0,
        e.yMark ? REMEMBERED_1 : REMEMBERED_0
    };
}

// A pointer colour, as coordinates rather than bits, so that "erase a family" is an operation on
// the model and not a bit-twiddling accident.
//
// Only states a colouring function can actually produce are enumerated. In particular there is
// no "no MarkedYoung bit at all" row: every ZGC colouring function stamps the current epoch
// (zAddress.inline.hpp:752-790), and our storeColour does the same. The mask predicate *would*
// admit such a word -- that is recorded once, below, as kMaskAdmitsUnstampedWord, rather than
// being folded into the table where it would read as a supported state.
struct Colour {
    unsigned rY;     // conceptual RemappedYoung epoch carried, 0|1
    unsigned rO;     // conceptual RemappedOld epoch carried, 0|1
    unsigned yMark;  // MarkedYoung epoch carried, 0|1
    unsigned oMark;  // MarkedOld / Finalizable epoch carried, 0|1
    unsigned fin;    // 0 = strongly marked old, 1 = finalizable-marked old
    unsigned rem;    // 0 = Remembered epoch0, 1 = epoch1, 2 = both bits (forgotten old-to-old)
    unsigned tagged; // unused: mid-evacuation is not a pointer bit
    unsigned ghost;  // redundant family: physically encoded, read by nothing
    unsigned shadow; // coordinate no encoding carries at all
};

// The enumerated space is the tagged == 0 half. Mid-evacuation references are covered by
// TaggedImpliesAllBad (masks) and TaggedRowsAreConstant (actions) instead of being crossed with
// every other coordinate: the action short-circuits on them before it reads any family, and
// TaggedRowsAreConstant is what checks that claim rather than asserting it in a comment.
constexpr unsigned kColourCount = 2u * 2u * 2u * 2u * 2u * 3u * 2u * 2u; // 384

constexpr Colour ColourAt(unsigned i)
{
    return Colour{ i % 2u,         (i / 2u) % 2u,  (i / 4u) % 2u,   (i / 8u) % 2u, (i / 16u) % 2u,
                   (i / 32u) % 3u, 0u,             (i / 96u) % 2u,  (i / 192u) % 2u };
}

constexpr Colour ColourAtTagged(unsigned i)
{
    return Colour{ i % 2u,         (i / 2u) % 2u,  (i / 4u) % 2u,   (i / 8u) % 2u, (i / 16u) % 2u,
                   (i / 32u) % 3u, 1u,             (i / 96u) % 2u,  (i / 192u) % 2u };
}

constexpr bool SameColour(Colour a, Colour b)
{
    return a.rY == b.rY && a.rO == b.rO && a.yMark == b.yMark && a.oMark == b.oMark && a.fin == b.fin &&
        a.rem == b.rem && a.tagged == b.tagged && a.ghost == b.ghost && a.shadow == b.shadow;
}

// The physical word a colour occupies under the live layout, used only to tie the model back to
// the product masks. ghost takes a spare padding bit; shadow has no bit by construction.
constexpr uintptr_t EncodeWord(Colour p)
{
    return (uintptr_t(1) << (REMAP_COLOUR_SHIFT + p.rY + 2u * p.rO)) | (p.yMark ? MARKED_YOUNG_1 : MARKED_YOUNG_0) |
        (p.fin ? (p.oMark ? FINALIZABLE_1 : FINALIZABLE_0) : (p.oMark ? MARKED_OLD_1 : MARKED_OLD_0)) |
        (p.rem == 2u ? REMEMBERED_MASK : (p.rem == 1u ? REMEMBERED_1 : REMEMBERED_0)) |
        (p.ghost ? (uintptr_t(1) << 62) : uintptr_t(0));
}

// Which family a configuration is missing. "Ghost" is the control for the drop machinery: a
// family that is physically present and that the action function never consults, so dropping it
// must change nothing. Without it, "every drop reports a collision" and "the checker is stuck at
// yes" look the same.
enum class Drop : unsigned {
    None = 0,
    Ghost,
    Remembered,
    MarkedYoung,
    MarkedOld,
    RemapSplit,
    Finalizable,
};

// π_C: erase the families configuration C does not carry, mapping every colour onto the one that
// configuration can actually represent. `shadow` is erased under every configuration -- no
// encoding carries it -- which is what makes Collisions(Drop::None) a real question: it asks
// whether the action function depends on anything the encoding does not hold.
constexpr Colour Project(Colour p, Drop d)
{
    Colour q = p;
    q.shadow = 0u;
    if (d == Drop::Ghost) {
        q.ghost = 0u;
    } else if (d == Drop::Remembered) {
        q.rem = 0u;
    } else if (d == Drop::MarkedYoung) {
        q.yMark = 0u;
    } else if (d == Drop::MarkedOld) {
        // Finalizable rides the old-mark family (same bits' epoch, same flip); with no old-mark
        // family there is nothing for it to qualify.
        q.oMark = 0u;
        q.fin = 0u;
    } else if (d == Drop::RemapSplit) {
        // One remap family instead of two: the old generation's epoch is no longer independently
        // representable, so it collapses onto whatever the young one says.
        q.rO = q.rY;
    } else if (d == Drop::Finalizable) {
        // This is the live configuration. Without the family, a resurrection-marked target is
        // indistinguishable from a strongly marked one -- which is exactly what the runtime does
        // today: LiveInfo.h:210 / Heap.cpp:76 OR markBitmap with resurrectBitmap into a single
        // liveness answer, and the reference is healed to plain mark-good either way.
        q.fin = 0u;
    }
    return q;
}

// What the collector must do with this reference at this barrier, in this epoch. This is the
// specification the table checks; every arm is anchored, and the two places where we knowingly
// differ from ZGC (the tagID ring, and Finalizable living in a side table) are called out.
enum class Act : unsigned {
    Fastpath = 0,
    RemapTagged,
    RemapYoung,
    RemapOld,
    RemapConsultYoungTable,
    MarkYoung,
    MarkOld,
    UpgradeStrong,
    Remember,
};

constexpr unsigned kEvLoad = 0u;
constexpr unsigned kEvMark = 1u;
constexpr unsigned kEvStore = 2u;
constexpr unsigned kEventCount = 3u;

constexpr Act Action(Colour p, unsigned ei, unsigned ev)
{
    const ModelEpoch e = ModelEpochAt(ei);
    // Mid-evacuation is not a pointer bit. The inject arms sit below this line
    // so that they perturb only the enumerated half.
    if (p.tagged != 0u) {
        return Act::RemapTagged;
    }
#ifdef MRT_C4TABLE_INJECT_ACTION
    // Positive control for Collisions(Drop::None): make the decision depend on a coordinate the
    // encoding does not carry. A collector that did this could not be implemented at all.
    if (p.shadow != 0u) {
        return Act::Remember;
    }
#endif
#ifdef MRT_C4TABLE_INJECT_GHOST
    // Positive control for Collisions(Drop::Ghost): consult the redundant family.
    if (p.ghost != 0u) {
        return Act::Remember;
    }
#endif
    const bool youngLoadGood = (p.rY == e.yRemap); // zAddress.inline.hpp:648-651
    const bool oldLoadGood = (p.rO == e.oRemap);   // zAddress.inline.hpp:653-656
    if (!youngLoadGood || !oldLoadGood) {
        // zBarrier.inline.hpp:110-137 remap_generation.
        if (oldLoadGood) {
            return Act::RemapYoung;
        }
        if (youngLoadGood) {
            return Act::RemapOld;
        }
        if (p.rem == 2u) {
            return Act::RemapOld; // both Remembered bits: old-to-old, forgotten
        }
        return Act::RemapConsultYoungTable;
    }
    // The load barrier's fast path is load-good (zBarrier.inline.hpp:373,466): a
    // finalizable-marked reference passes it, which is why the finalizable question is a
    // mark-barrier question and not a load-barrier one.
    if (ev == kEvLoad) {
        return Act::Fastpath;
    }
    // Mark and store both gate on mark-good first (zBarrier.inline.hpp:377,471,620-622;
    // Collector.h:191 is_mark_good). Finalizable bits are inside MarkMetadata but outside
    // MarkGood (zAddress.hpp:192, zAddress.cpp:82), so a finalizable-marked reference is
    // permanently mark-bad.
    const bool markGood = (p.fin == 0u) && (p.yMark == e.yMark) && (p.oMark == e.oMark);
    if (!markGood) {
        if (p.fin != 0u) {
            // zBarrier.inline.hpp:610-620: the whole reason the bit exists.
            return Act::UpgradeStrong;
        }
        if (p.yMark != e.yMark) {
            return Act::MarkYoung;
        }
        return Act::MarkOld;
    }
    if (ev == kEvMark) {
        return Act::Fastpath;
    }
    // Store: store-good adds the current Remembered epoch (zAddress.cpp:83). Carrying both bits
    // is store-bad as well, which is how "forgotten" forces the slow path.
    if (p.rem != e.yMark) {
        return Act::Remember;
    }
    return Act::Fastpath;
}

// UNCERTAIN(C): (epoch, event, pair) triples where two colours that configuration C cannot tell
// apart demand different actions. No branch anywhere writes this down; it is counted.
constexpr unsigned Collisions(Drop d)
{
    unsigned n = 0;
    for (unsigned ei = 0; ei < kEpochCount; ++ei) {
        for (unsigned ev = 0; ev < kEventCount; ++ev) {
            for (unsigned i = 0; i < kColourCount; ++i) {
                const Colour p = ColourAt(i);
                const Colour q = Project(p, d);
                if (SameColour(p, q)) {
                    continue;
                }
                if (Action(p, ei, ev) != Action(q, ei, ev)) {
                    ++n;
                }
            }
        }
    }
    return n;
}

// Every mid-evacuation row takes the same exit, whatever colour it carries and whatever epoch it
// is judged in. This is why the tagged half is not crossed with the rest of the table: it cannot
// contribute a collision, and here that is checked rather than argued.
constexpr bool TaggedRowsAreConstant()
{
    bool ok = true;
    for (unsigned ei = 0; ei < kEpochCount; ++ei) {
        for (unsigned ev = 0; ev < kEventCount; ++ev) {
            for (unsigned i = 0; i < kColourCount; ++i) {
                if (Action(ColourAtTagged(i), ei, ev) != Act::RemapTagged) {
                    ok = false;
                }
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Part 3 -- the model is tied to the product's own masks
// ---------------------------------------------------------------------------
// Without this the table would be a self-consistent story about a formula nobody publishes.
// Restricted to fin == 0, i.e. the colours the live layout can represent: see
// kLiveMarkMaskCannotSeeFinalizable below for the other half.
constexpr bool ModelAgreesWithLiveMasks()
{
    bool ok = true;
    for (unsigned ei = 0; ei < kEpochCount; ++ei) {
        const ModelEpoch e = ModelEpochAt(ei);
        const BadMasks m = ComputeBadMasks(EpochColoursOf(e));
        for (unsigned k = 0; k < kColourCount; ++k) {
            const Colour p = ColourAt(k);
            if (p.fin != 0u || p.ghost != 0u || p.shadow != 0u || p.tagged != 0u) {
                continue;
            }
            const uintptr_t w = EncodeWord(p);
            const bool modelLoadGood = (p.tagged == 0u) && (p.rY == e.yRemap) && (p.rO == e.oRemap);
            const bool modelMarkGood = modelLoadGood && (p.yMark == e.yMark) && (p.oMark == e.oMark);
            const bool modelStoreGood = modelMarkGood && (p.rem == e.yMark);
            if (modelLoadGood != ((w & m.loadBad) == 0)) {
                ok = false;
            }
            if (modelMarkGood != ((w & m.markBad) == 0)) {
                ok = false;
            }
            if (modelStoreGood != ((w & m.storeBad) == 0)) {
                ok = false;
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Part 4 -- the witness pair for the family we do not have
// ---------------------------------------------------------------------------
// A hand-set `uncertain = true` cannot produce a concrete pair of colours that a configuration
// cannot separate; this can, and the three assertions below are the definition of "collision",
// checked by the compiler rather than printed.
struct Witness {
    Colour p1;
    Colour p2;
    unsigned ei;
    unsigned ev;
};

// Epoch 0: young/old remap epoch 0, MarkedYoung epoch 0, MarkedOld epoch 0, Remembered epoch 0.
// p1: load-good, stamped with the current young and old mark epochs -> mark-good -> fast path.
//     Our shape of it: the target's markBitmap bit is set (LiveInfo.h:48-117).
// p2: identical, except the old-mark stamp is finalizable rather than strong.
//     Our shape of it: the target's resurrectBitmap bit is set and markBitmap is clear
//     (LiveInfo.h:204), which DoResurrection produces inside the concurrent marking segment
//     (TracingCollector.cpp:680-698).
// ZGC answers UpgradeStrong here (zBarrier.inline.hpp:610-620). We answer Fastpath, because the
// two colours are the same colour.
constexpr Witness kWitnessFin = { Colour{ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
                                  Colour{ 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u }, 0u, kEvMark };

// ---------------------------------------------------------------------------
// Part 5 -- recorded divergences: stated, checked, NOT changed by this commit
// ---------------------------------------------------------------------------

// (a) A non-null word carrying no metadata at all is load-good today. Collector.h:132-140 says
//     so deliberately, following zAddress.inline.hpp:635-643. Narrowing it is a behaviour change
//     that pushes those values onto a slow path which has nothing to say about them -- i.e. it
//     trades a silent admit for a silent drop. Recorded here so the next person finds it as a
//     fact with a line number rather than as a rumour.
constexpr uintptr_t kPlainNonNullWord = 0x1000u;
constexpr bool kPlainNonNullIsLoadGoodToday =
    (kPlainNonNullWord & ComputeBadMasks(MapleRuntime::kInitialEpochColours).loadBad) == 0;

// (b) The mask predicate also admits a word that carries remap colour but no mark stamp at all.
//     No colouring function produces one, which is why the table does not enumerate it.
constexpr uintptr_t kUnstampedWord = ZPointerRemapped00 | REMEMBERED_0;
constexpr bool kMaskAdmitsUnstampedWord =
    (kUnstampedWord & ComputeBadMasks(MapleRuntime::kInitialEpochColours).markBad) == 0;

// (c) The gap this whole probe exists to name, in one expression: no live mask can see the
//     Finalizable family, so no barrier can be forced down a slow path by it.
constexpr bool kLiveMarkMaskCannotSeeFinalizable =
    (FINALIZABLE_MASK & ComputeBadMasks(MapleRuntime::kInitialEpochColours).markBad) == 0 &&
    (FINALIZABLE_MASK & ComputeBadMasks(MapleRuntime::kInitialEpochColours).storeBad) == 0 &&
    !MapleRuntime::kFinalizableWired;

// ---------------------------------------------------------------------------
// The judgements
// ---------------------------------------------------------------------------

static_assert(MaskEquivAllEpochs(),
              "C4TABLE_MASK_EQUIV: ComputeBadMasks disagrees with the literal pre-C4 expression on "
              "at least one of the 32 epochs. The lifted formula is not the formula that shipped.");
static_assert(LatticeOrderHolds(),
              "C4TABLE_LATTICE: store-bad no longer contains mark-bad, or mark-bad no longer contains "
              "load-bad, on some epoch.");
static_assert(TaggedImpliesAllBad(),
              "C4TABLE_TAGGED: TAGGED_BITS_MASK is no longer identically 0, or a zero term is not "
              "contained in every published mask.");
static_assert(TAGGED_BITS_MASK == 0,
              "C4TABLE_TAGGED: pointer layout reintroduced isTagged/tagID.");
static_assert(kEvStore == 2u && kEventCount == 3u, "C4TABLE: event set changed.");
static_assert(ModelAgreesWithLiveMasks(),
              "C4TABLE_MODEL_TIE: the action function's good/bad predicates no longer match the masks "
              "ComputeBadMasks publishes. The table would be describing a collector we do not ship.");

static_assert(Collisions(Drop::None) == 0u,
              "C4TABLE_DROP_NONE: the action depends on state no encoding carries, so no colour "
              "assignment can implement it.");
static_assert(Collisions(Drop::Ghost) == 0u,
              "C4TABLE_DROP_GHOST: dropping a family nothing reads produced an ambiguity, so the "
              "collision check is reporting yes regardless of its input.");
static_assert(Collisions(Drop::Remembered) > 0u,
              "C4TABLE_DROP_REMEMBERED: the Remembered family is not load-bearing after all -- either "
              "the action function stopped consulting it, or it is genuinely redundant.");
static_assert(Collisions(Drop::MarkedYoung) > 0u, "C4TABLE_DROP_MARKED_YOUNG: family no longer load-bearing.");
static_assert(Collisions(Drop::MarkedOld) > 0u, "C4TABLE_DROP_MARKED_OLD: family no longer load-bearing.");
static_assert(Collisions(Drop::RemapSplit) > 0u,
              "C4TABLE_DROP_REMAP_SPLIT: one remap family would now suffice for two generations.");
static_assert(Collisions(Drop::Finalizable) > 0u,
              "C4TABLE_DROP_FINALIZABLE: the fifth family is no longer needed -- which would mean the "
              "resurrection state became representable some other way. Say where.");

// The witness is checked, not narrated: two different colours, identical under the live
// projection, demanding different actions.
static_assert(!SameColour(kWitnessFin.p1, kWitnessFin.p2), "C4TABLE_WITNESS: the witness pair is one colour.");
static_assert(SameColour(Project(kWitnessFin.p1, Drop::Finalizable), Project(kWitnessFin.p2, Drop::Finalizable)),
              "C4TABLE_WITNESS: the witness pair is distinguishable without the Finalizable family, so it "
              "witnesses nothing.");
static_assert(Action(kWitnessFin.p1, kWitnessFin.ei, kWitnessFin.ev) !=
                  Action(kWitnessFin.p2, kWitnessFin.ei, kWitnessFin.ev),
              "C4TABLE_WITNESS: the witness pair agrees on the action, so there is no ambiguity to "
              "witness.");
static_assert(Action(kWitnessFin.p1, kWitnessFin.ei, kWitnessFin.ev) == Act::Fastpath,
              "C4TABLE_WITNESS: a strongly marked, mark-good reference should take the fast path.");
static_assert(Action(kWitnessFin.p2, kWitnessFin.ei, kWitnessFin.ev) == Act::UpgradeStrong,
              "C4TABLE_WITNESS: a finalizable-marked reference should be upgraded to strong.");

// Recorded, not enforced as desirable: these three say what is true today.
static_assert(kPlainNonNullIsLoadGoodToday, "KNOWN_DIVERGENCE (a) changed; that is a behaviour change.");
static_assert(kMaskAdmitsUnstampedWord, "KNOWN_DIVERGENCE (b) changed; that is a behaviour change.");
static_assert(kLiveMarkMaskCannotSeeFinalizable,
              "KNOWN_DIVERGENCE (c): the Finalizable family became visible to a live mask. That is C4 "
              "knife 6 -- a real behaviour change and a two-half pin bump.");

} // namespace

#ifdef MRT_C4TABLE_PRINT_WITNESS
// Manual arm only (runtime/tests/run_colour_table_witness.sh). Prints for a human; the judgement
// is the static_asserts above, which have already run by the time this compiles.
#include <cstdio>

namespace {
void PrintColour(const char* tag, Colour p)
{
    std::printf("  %s rY=%u rO=%u yMark=%u oMark=%u fin=%u rem=%u tagged=%u ghost=%u shadow=%u word=%#lx\n", tag, p.rY,
                p.rO, p.yMark, p.oMark, p.fin, p.rem, p.tagged, p.ghost, p.shadow,
                static_cast<unsigned long>(EncodeWord(p)));
}
} // namespace

int main()
{
    std::printf("C4TABLE colours=%u epochs=%u events=%u\n", kColourCount, kEpochCount, kEventCount);
    std::printf("C4TABLE collisions None=%u Ghost=%u Remembered=%u MarkedYoung=%u MarkedOld=%u "
                "RemapSplit=%u Finalizable=%u\n",
                Collisions(Drop::None), Collisions(Drop::Ghost), Collisions(Drop::Remembered),
                Collisions(Drop::MarkedYoung), Collisions(Drop::MarkedOld), Collisions(Drop::RemapSplit),
                Collisions(Drop::Finalizable));
    std::printf("C4TABLE witness (epoch=%u event=%u) Finalizable:\n", kWitnessFin.ei, kWitnessFin.ev);
    PrintColour("p1(strongly marked old)  ", kWitnessFin.p1);
    PrintColour("p2(finalizable marked old)", kWitnessFin.p2);
    std::printf("  action(p1)=%u action(p2)=%u  projected-equal=%d\n",
                static_cast<unsigned>(Action(kWitnessFin.p1, kWitnessFin.ei, kWitnessFin.ev)),
                static_cast<unsigned>(Action(kWitnessFin.p2, kWitnessFin.ei, kWitnessFin.ev)),
                static_cast<int>(SameColour(Project(kWitnessFin.p1, Drop::Finalizable),
                                            Project(kWitnessFin.p2, Drop::Finalizable))));
    const BadMasks m = ComputeBadMasks(MapleRuntime::kInitialEpochColours);
    std::printf("C4TABLE initial masks remap=%#lx load=%#lx mark=%#lx store=%#lx finalizable_mask=%#lx\n",
                static_cast<unsigned long>(m.remapColour), static_cast<unsigned long>(m.loadBad),
                static_cast<unsigned long>(m.markBad), static_cast<unsigned long>(m.storeBad),
                static_cast<unsigned long>(FINALIZABLE_MASK));
    return 0;
}
#endif
